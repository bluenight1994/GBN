#include "gbn.h"

state_t s;
extern struct sockaddr_in remote;
extern socklen_t rl;

static void alarmHandler(int signo) {
	printf("timeout\n");
	return;
}

uint16_t checksum(uint16_t *buf, int nwords)
{
	uint32_t sum;

	for (sum = 0; nwords > 0; nwords--)
		sum += *buf++;
	sum = (sum >> 16) + (sum & 0xffff);
	sum += (sum >> 16);
	return ~sum;
}

ssize_t gbn_send(int sockfd, const void *buf, size_t len, int flags){
	
	int n_seg = ceil(len / (double)DATALEN), send_bytes = 0;

	int i = 0;
	// printf("n_seg: %d\n", n_seg);

	for (i = 0; i < n_seg; i++) {
		int trials;
		for (trials = 0; trials < 5; trials++) {
			gbnhdr t_hdr;
			t_hdr.type = DATA;
			t_hdr.seqnum = s.seq_num;
			t_hdr.checksum = 0;
			t_hdr.datalen = min(len, DATALEN);
			memcpy(t_hdr.data, buf + i * DATALEN, min(len, DATALEN));
			//printf("client send data: %s\n", t_hdr.data);
			int t_sum = checksum((uint16_t *) &t_hdr, sizeof(t_hdr) / 2);
			t_hdr.checksum = t_sum;

			int t_send_ret = sendto(sockfd, &t_hdr, sizeof(t_hdr), 0, (struct sockaddr *) &remote, rl);
			if (t_send_ret < 0) {
				printf("gbn_send: client send data error\n");
				return -1;
			}
			printf("gbn_send: client send %d bytes data\n", t_send_ret);
			/* set time out for receiving DATA ACK */
			alarm(TIMEOUT);
			struct sigaction sact = {
    			.sa_handler = alarmHandler,
    			.sa_flags = 0,
			};
			sigaction(SIGALRM, &sact, NULL);
			gbnhdr tmp;
			int t_ret;
			t_ret = recvfrom(sockfd, &tmp, sizeof(tmp), 0, (struct sockaddr *) &remote, &rl);
			printf("gbn_send: signal type: %d\n", tmp.type);
			if (t_ret < 0) {
				if (errno == EINTR) {
					printf("gbn_send: recv data ack time out\n");
				} else {
					printf("gbn_send: recvfrom data ack error");
				}
				// current data is missing, need to retransmit.
				trials--;
			} else {
				alarm(0);
				s.seq_num++;
				send_bytes += min(len, DATALEN);
				len -= DATALEN;
				break;
			}
		}
	}
	printf("data send success and time to close connection\n");
	return send_bytes;
}

ssize_t gbn_recv(int sockfd, void *buf, size_t len, int flags){

	printf("server start to receive\n");
	gbnhdr tmp;
	socklen_t tt = sizeof(struct sockaddr);
	ssize_t t_recv_ret = recvfrom(sockfd, &tmp, sizeof(tmp), 0, (struct sockaddr *)&remote, &tt);
	rl = tt;
	printf("server receiver %zd\n", t_recv_ret);
	if (t_recv_ret < 0) {
		return -1;
	}

	tmp.checksum = 0;
	if (tmp.type == DATA) {
		printf("data length: %d\n", tmp.datalen);
		memcpy(buf, tmp.data, tmp.datalen);
		s.seq_num = tmp.seqnum;
		memcpy(buf, tmp.data, tmp.datalen);

		gbnhdr t_hdr;
		t_hdr.type = DATAACK;
		t_hdr.seqnum = s.seq_num;
		t_hdr.checksum = 0;
		int t_send_ret;
		t_send_ret = sendto(sockfd, &t_hdr, sizeof(t_hdr), 0, (struct sockaddr *)&remote, rl);
		if (t_send_ret < 0) {
			printf("send data ack error\n");
			return -1;
		}
		printf("server receiver %d\n", t_hdr.datalen);
		return tmp.datalen;
	}
	if (tmp.type == FIN) {
		gbnhdr t_hdr;
		t_hdr.type = FINACK;
		t_hdr.seqnum = s.seq_num;
		t_hdr.checksum = 0;
		sendto(sockfd, &t_hdr, sizeof(t_hdr), 0, (struct sockaddr *)&remote, rl);
	}
	return 0;
}

int gbn_close(int sockfd){

	if (s.role == 1) {
		gbnhdr t_hdr;
		t_hdr.type = FIN;
		t_hdr.seqnum = 0;
		t_hdr.checksum = 0;
		sendto(sockfd, &t_hdr, sizeof(t_hdr), 0, (struct sockaddr *)&remote, rl);
		printf("sender FIN packet sent\n");
		return(0);
	}

	return 0;
}

int gbn_connect(int sockfd, const struct sockaddr *server, socklen_t socklen){

	s.role = 1;
	s.seq_num = 0;

	memcpy(&remote, server, socklen);
	rl = socklen;

	/* integrated with time out module */

	printf("gbn_connect: client try to connect server, start to send syn\n");

	for (int i = 0; i < 5; i++) {
		printf("gbn_connect: trial times %d\n", i);
		gbnhdr t_hdr;
		t_hdr.type = SYN;
		t_hdr.seqnum = s.seq_num;
		t_hdr.checksum = 0;
		int t_send_ret;
		t_send_ret = sendto(sockfd, &t_hdr, sizeof(t_hdr), 0, server, socklen);
		if (t_send_ret < 0) {
			printf("gbn_connect: client send syn error\n");
			return (-1);
		}
		printf("gbn_connect: client send syn success, expecting to recv SYNACK\n");
		gbnhdr tmp;
		int t_ret;
		alarm(TIMEOUT);
		struct sigaction sact = {
    		.sa_handler = alarmHandler,
    		.sa_flags = 0,
		};
		sigaction(SIGALRM, &sact, NULL);
		// signal(SIGALRM, alarmHandler);
		t_ret = recvfrom(sockfd, &tmp, sizeof(tmp), 0, server, &socklen);
		if (t_ret < 0) {
			// printf("errno: %d\n", errno);
			if (errno == EINTR) {
				printf("gbn_connect: recvfrom time out\n");
			} else {
				printf("gbn_connect: recvfrom error\n");
			}
		} else {
			alarm(0);
			s.seq_num += 1;
			if (tmp.type == SYNACK) {
				printf("gbn_connect: client recv synack success and connection established\n");
				s.state = ESTABLISHED;
				return 0;
			}
		}
	}
	return(-1);
}

int gbn_listen(int sockfd, int backlog) {

	s.seq_num = 0;
	return(0);
}

int gbn_bind(int sockfd, const struct sockaddr *server, socklen_t socklen) {

	return bind(sockfd, server, socklen);
}	

int gbn_socket(int domain, int type, int protocol) {
		
	/*----- Randomizing the seed. This is used by the rand() function -----*/
	srand((unsigned)time(0));
	
	int sockfd;
	sockfd = socket(domain, type, protocol);
	return sockfd;
}

int gbn_accept(int sockfd, struct sockaddr *client, socklen_t *socklen) {

	gbnhdr t_recv_hdr;
	int t_recv_ret;
	t_recv_ret = recvfrom(sockfd, &t_recv_hdr, sizeof(t_recv_hdr), 0, client, socklen);
	if (t_recv_ret < 0) {
		printf("gbn_accept: server receive syn error\n");
	}
	
	memcpy(&remote, client, *socklen);
	rl = *socklen;

	if (t_recv_hdr.type == SYN) {
		printf("gbn_accept: server receive syn success, send back synack\n");
		gbnhdr t_hdr;
		t_hdr.type = SYNACK;
		t_hdr.seqnum = 0;
		t_hdr.checksum = 0;
		int t_send_ret;
		t_send_ret = sendto(sockfd, &t_hdr, sizeof(t_hdr), 0, &remote, rl);
		if (t_send_ret < 0) {
			printf("gbn_accept: server send synack error\n");
			return (-1);
		}
		printf("gbn_accept: server send synack success and connection established\n");
		s.state = ESTABLISHED;
		return 0;
	}
	return sockfd;
}

ssize_t maybe_sendto(int  s, const void *buf, size_t len, int flags, \
                     const struct sockaddr *to, socklen_t tolen) {

	char *buffer = malloc(len);
	memcpy(buffer, buf, len);
	
	
	/*----- Packet not lost -----*/
	if (rand() > LOSS_PROB*RAND_MAX){
		/*----- Packet corrupted -----*/
		if (rand() < CORR_PROB*RAND_MAX){
			
			/*----- Selecting a random byte inside the packet -----*/
			int index = (int)((len-1)*rand()/(RAND_MAX + 1.0));

			/*----- Inverting a bit -----*/
			char c = buffer[index];
			if (c & 0x01)
				c &= 0xFE;
			else
				c |= 0x01;
			buffer[index] = c;
		}

		/*----- Sending the packet -----*/
		int retval = sendto(s, buffer, len, flags, to, tolen);
		free(buffer);
		return retval;
	}
	/*----- Packet lost -----*/
	else
		return(len);  /* Simulate a success */
}
