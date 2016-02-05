#include <uapi/linux/ptrace.h>
#include <net/sock.h>
#include <bcc/proto.h>

#define IP_TCP 	6   
#define ETH_HLEN 14

struct Key {
	u32 src_ip;               //source ip
	u32 dst_ip;               //destination ip
	unsigned short src_port;  //source port
	unsigned short dst_port;  //destination port	
};

struct Leaf {
	int timestamp;            //timestamp in ns
};

//BPF_TABLE(map_type, key_type, leaf_type, table_name, num_entry) 
//map <Key, Leaf>
//tracing sessions having same Key(dst_ip, src_ip, dst_port,src_port)
BPF_TABLE("hash", struct Key, struct Leaf, sessions, 1024);

/*eBPF program.
  Filter IP and TCP packets, having payload not empty
  and containing "HTTP", "GET", "POST"  as first bytes of payload.
  AND ALL the other packets having same (src_ip,dst_ip,src_port,dst_port)
  this means belonging to the same "session"
  this additional check avoids url truncation, if url is too long
  userspace script, if necessary, reassembles urls splitted in 2 or more packets.
  if the program is loaded as PROG_TYPE_SOCKET_FILTER
  and attached to a socket
  return  0 -> DROP the packet
  return -1 -> KEEP the packet and return it to user space (userspace can read it from the socket_fd )
*/
int http_filter(struct __sk_buff *skb) {

	u8 *cursor = 0;

	struct ethernet_t *ethernet = cursor_advance(cursor, sizeof(*ethernet));
	//filter IP packets (ethernet type = 0x0800)
	if (!(ethernet->type == 0x0800)){
		goto DROP;	
	}

	struct ip_t *ip = cursor_advance(cursor, sizeof(*ip));
	//filter TCP packets (ip next protocol = 0x06)
	if (ip->nextp != IP_TCP) {
		goto DROP;
	}

	u32  tcp_header_length = 0;
	u32  ip_header_length = 0;
	u32  payload_offset = 0;
	u32  payload_length = 0;
	struct Key 	key;
	struct Leaf leaf;

	struct tcp_t *tcp = cursor_advance(cursor, sizeof(*tcp));

	//retrieve ip src/dest and port src/dest of current packet
	//and save it into struct Key
	key.dst_ip = ip->dst;
	key.src_ip = ip->src;
	key.dst_port = tcp->dst_port;
	key.src_port = tcp->src_port;

	//calculate ip header length
	//value to multiply * 4
	//e.g. ip->hlen = 5 ; IP Header Length = 5 x 4 byte = 20 byte
	ip_header_length = ip->hlen << 2;    //SHL 2 -> *4 multiply
		
	//calculate tcp header length
	//value to multiply *4
	//e.g. tcp->offset = 5 ; TCP Header Length = 5 x 4 byte = 20 byte
	tcp_header_length = tcp->offset << 2; //SHL 2 -> *4 multiply

	//calculate patload offset and lenght
	payload_offset = ETH_HLEN + ip_header_length + tcp_header_length; 
	payload_length = ip->tlen - ip_header_length - tcp_header_length;
		  
	//http://stackoverflow.com/questions/25047905/http-request-minimum-size-in-bytes
	//minimum lenght of http request is always geater than 7 bytes
	//avoid invalid access memory
	//include empty payload
	if(payload_length < 7){
		goto DROP;
	}

	//load firt 7 byte of payload into payload_array
	//direct access to skb not allowed
	unsigned long payload_array[7];
	int i = 0;
	int j = 0;
	for (i = payload_offset ; i < (payload_offset + 7) ; i++){
		payload_array[j] = load_byte(skb , i);
		j++;
	}

	//find a match with an HTTP message
	//HTTP
	if ( (payload_array[0] == 'H') && (payload_array[1] == 'T') && (payload_array[2] == 'T') && (payload_array[3] == 'P')){
		goto HTTP_MATCH;
	}
	//GET
	if ( (payload_array[0] == 'G') && (payload_array[1] == 'E') && (payload_array[2] == 'T') ){
		goto HTTP_MATCH;
	}
	//POST
	if ( (payload_array[0] == 'P') && (payload_array[1] == 'O') && (payload_array[2] == 'S') && (payload_array[3] == 'T')){
		goto HTTP_MATCH;
	}
	//PUT
	if ( (payload_array[0] == 'P') && (payload_array[1] == 'U') && (payload_array[2] == 'T') ){
		goto HTTP_MATCH;
	}
	//DELETE
	if ( (payload_array[0] == 'D') && (payload_array[1] == 'E') && (payload_array[2] == 'L') && (payload_array[3] == 'E') && (payload_array[4] == 'T') && (payload_array[5] == 'E')){
		goto HTTP_MATCH;
	}
	//HEAD
	if ( (payload_array[0] == 'H') && (payload_array[1] == 'E') && (payload_array[2] == 'A') && (payload_array[3] == 'D')){
		goto HTTP_MATCH;
	}

	//no HTTP match
	//check if packet belong to an HTTP session
	struct Leaf * lookup_leaf = sessions.lookup(&key);
	if(lookup_leaf){
		//send packet to userspace
		goto KEEP;
	}
	goto DROP;

	//keep the packet and send it to userspace retruning -1
	HTTP_MATCH:
	//if not already present, insert into map <Key, Leaf>
	leaf.timestamp = 0;
	sessions.lookup_or_init(&key, &leaf);
	sessions.update(&key,&leaf);
	
	//send packet to userspace returning -1
	KEEP:
	return -1;

	//drop the packet returning 0
	DROP:
	return 0;

}
