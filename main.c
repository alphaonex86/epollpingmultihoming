#include <time.h>
#include <sys/timerfd.h>
#include <getopt.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <netinet/ip_icmp.h>
#include <netinet/ip.h>
#include <netdb.h>
#include <errno.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <libgen.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_EVENTS 16

unsigned char *header = NULL;
unsigned short seq=0;
struct _host *ipList=NULL;
unsigned int hostcount=0;
char str[INET_ADDRSTRLEN];
struct _packet packet;
bool priority=false;
bool help=false;
bool average=false;
bool noscript=false;

struct _packet
{
    struct icmphdr hdr;
    unsigned char msg[64];
};
struct _host
{
    char *address;
    struct sockaddr_in *socket;
    bool replyReceived;
    bool lastState;
    int sd;
    bool results[100];
    unsigned int resultsCount;
    unsigned int resultSuccess;
};

char *gettime()
{
    time_t t;
    time(&t);
    char* p = ctime(&t);
    const size_t l=strlen(p);
    p[l-1]='\0';
    return p;
}
void term(int signum)
{
    switch(signum)
    {
        case SIGTERM:
            printf("[%s] SIGTERM\n", gettime());
            break;
        case SIGINT:
            printf("[%s] SIGINT\n", gettime());
            break;
        default:
            printf("[%s] Signal %i\n", gettime(), signum);
            break;
    }
    exit(0);
}
unsigned short checksum(void *b, int len)
{
    unsigned short *buf = b;
    unsigned int sum=0;
    unsigned short result;

    for (sum = 0; len > 1; len -= 2 )
        sum += *buf++;
    if (len == 1 )
        sum += *(unsigned char*)buf;
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    result = ~sum;
    return result;
}

void parseReply(void *buf)
{
    struct iphdr   *ip   = buf;
    struct icmphdr *icmp = buf + ip->ihl * 4;
    const uint16_t sequence=htons(icmp->un.echo.sequence);
    if(seq!=sequence)
        return;
    unsigned int index=0;
    while(index<hostcount)
    {
        if(memcmp(&ipList[index].socket->sin_addr,&ip->saddr,sizeof(ip->saddr))==0)
        {
            ipList[index].replyReceived=true;
            break;
        }
        index++;
    }
    if(index==hostcount)
    {
        printf("unknown reply detect\n");
        //exit(1);
        return;
    }
}

void ping(struct sockaddr_in *addr, const int sd/*struct protoent *proto*/, unsigned short cnt/*, unsigned short hostid*/)
{
    packet.hdr.checksum = 0;
    packet.hdr.un.echo.sequence = htons(cnt);
    packet.hdr.checksum = checksum(&packet, sizeof(packet));

    if (sendto(sd, &packet, sizeof(packet), 0, (struct sockaddr*)addr, sizeof(*addr)) <= 0 )
    {
        //perror("sendto");
    }
}


int main (int argc, char *argv[])
{
    printf("[%s] Start\n", gettime());

    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = term;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    hostcount=argc-1;
    unsigned char buf[1024];
    ipList = malloc(sizeof(struct _host) * hostcount);

    //resolv proto and pid
    int pid = getpid();
    struct protoent *proto =  getprotobyname("ICMP");

    if(argc<2) {
        printf("argument count error\n");
        exit(1);
    }

    dirname(argv[0]);
    strcat(argv[0], "/");
    printf("path=%s\n", argv[0]);

    //parse text
    char *name = "CONFIARED";
    unsigned char *header = malloc(strlen(name));
    for (unsigned int i = 0; i < strlen(name); i++)
        header[i] = name[i] + '0';

    //parse the ip
    char **hostlist=&argv[1];
    unsigned indexIpList=0;
    for (unsigned int i = 0; i < hostcount; i++) {
        if(hostlist[i][0]=='-')
        {
            if(hostlist[i][1]=='-')
            {
                if(strcmp(hostlist[i],"--help")==0)
                    help=true;
                else if(strcmp(hostlist[i],"--priority")==0)
                    priority=true;
                else if(strcmp(hostlist[i],"--average")==0)
                    average=true;
                else if(strcmp(hostlist[i],"--noscript")==0)
                    noscript=true;
                else
                {
                    printf("unknown argument: %s",hostlist[i]);
                    help=true;
                }
            }
            else
            {
                int index=1;
                while(hostlist[i][index]!='\0')
                {
                    if(hostlist[i][index]=='h')
                        help=true;
                    else if(hostlist[i][index]=='p')
                        priority=true;
                    else if(hostlist[i][index]=='a')
                        average=true;
                    else if(hostlist[i][index]=='n')
                        noscript=true;
                    else
                    {
                        printf("unknown argument: %s",hostlist[i]);
                        help=true;
                    }
                    index++;
                }
            }
        }
        else
        {
            ipList[indexIpList].address=hostlist[i];
            ipList[indexIpList].socket=malloc(sizeof(struct sockaddr_in));
            ipList[indexIpList].lastState=false;
            memset(ipList[indexIpList].results,0,sizeof(ipList[indexIpList].results));
            ipList[indexIpList].resultsCount=0;
            ipList[indexIpList].resultSuccess=0;
            memset(ipList[indexIpList].socket, 0, sizeof(*ipList[indexIpList].socket));
            const int convertResult=inet_pton(AF_INET,ipList[indexIpList].address,&ipList[indexIpList].socket->sin_addr);
            if(convertResult!=1)
            {
                   printf("not an IPv4");
                   exit(1);
            }
            else
                ipList[indexIpList].socket->sin_family = AF_INET;

            if ((ipList[indexIpList].sd = socket(PF_INET, SOCK_RAW|SOCK_NONBLOCK, proto->p_proto)) < 0) {
                perror("socket");
                return -1;
            }
            const int ttl  = 61;
            if (setsockopt(ipList[indexIpList].sd, SOL_IP, IP_TTL, &ttl, sizeof(ttl)) != 0)
                perror("Set TTL option");
            if (fcntl(ipList[indexIpList].sd, F_SETFL, O_NONBLOCK) != 0 )
                perror("Request nonblocking I/O");

            indexIpList++;
        }
    }
    if(help)
    {
        printf("usage: ./epollpingmultihoming [-h] [-p] [-a] [-n] ip <.. ip>\n");
        printf("-h     --help to show this help\n");
        printf("-p     --priority to the first have more priority, when back online switch to it\n");
        printf("-a     --average choice when have lower ping lost average, if near lost then use priority if -p defined\n");
        printf("-n     --noscript don't call external script\n");
        return -1;
    }
    
    printf("priority: %i, average: %i, noscript: %i\n", priority, average, noscript);
    hostcount=indexIpList;

    //add main sd
    struct sockaddr_in addrmain;
    int sdmain=0;
    if ((sdmain = socket(PF_INET, SOCK_RAW|SOCK_NONBLOCK, proto->p_proto)) < 0 ) {
        printf("failed to open listening socket: %s", strerror(errno));
        exit(1);
    }
    struct epoll_event evmain;
    memset(&evmain,0,sizeof(evmain));
    evmain.events = EPOLLIN;
    evmain.data.fd = sdmain;

    //prepare the packet
    const uint16_t ver = 1;
    memset(&packet, 0, sizeof(packet));
    packet.hdr.type       = ICMP_ECHO;
    packet.hdr.un.echo.id = htons(pid);
    memcpy(packet.msg,      header,  5);
    memcpy(packet.msg + 5,  &ver,    1);
    memcpy(packet.msg + 6,  &ver, 2);
    unsigned long org_s  = 0;
    unsigned long org_ns = 0;
    //hostid = htons(hostid);
    //memcpy(pckt.msg + 6,  &hostid, 2);
    memcpy(packet.msg + 8,  &org_s,  4);
    memcpy(packet.msg + 12, &org_ns, 4);

    //pthread_setname_np(listen_thread, "listener");
    int fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);

    //timer to ping at interval
    struct itimerspec it;
    it.it_interval.tv_sec  = 10;
    it.it_interval.tv_nsec = 0;
    it.it_value.tv_sec     = it.it_interval.tv_sec;
    it.it_value.tv_nsec    = it.it_interval.tv_nsec;

    //the event loop
    struct epoll_event ev, events[MAX_EVENTS];
    memset(&ev,0,sizeof(ev));
    int nfds, epollfd;

    ev.events = EPOLLIN|EPOLLET;
    uint64_t value;

    if ((epollfd = epoll_create1(0)) == -1) {
        printf("epoll_create1: %s", strerror(errno));
        exit(1);
    }

    //timer event
    timerfd_settime(fd, 0, &it, NULL);
    ev.data.fd = fd;

    //add to event loop
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        printf("epoll_ctl failed to add timerfd: %s", strerror(errno));
        exit(1);
    }
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, sdmain, &evmain) == -1)
        fprintf(stderr, "epoll_ctl sfd");

    const char* scriptbase = "up.sh ";
    char* full = NULL;
    int lastUpIP=-1;
    bool firstPing=true;
    int callSkip=0;
    for (;;) {
        if ((nfds = epoll_wait(epollfd, events, MAX_EVENTS, -1)) == -1)
            printf("epoll_wait error %s", strerror(errno));

        for (int n = 0; n < nfds; ++n) {
            if (events[n].data.fd == fd) {
                if (read(fd, &value, sizeof(uint64_t)) == -1) {
                    printf("failed to read timer, %s", strerror(errno));
                    exit(1);
                }
                if(!firstPing)
                    seq++;
                int firstUpIP=-1;
                //sort if needed, output 0 to hostcount-1
                unsigned int indexOrdered[hostcount];
                for (unsigned int z = 0; z < hostcount; z++)
                    indexOrdered[z]=z;
                if(priority || average)
                {
                    for (unsigned int c = 0 ; c < hostcount - 1; c++)
                    {
                        for (unsigned int d = 0 ; d < hostcount - c - 1; d++)
                        {
                            struct _host * hostA=&ipList[indexOrdered[d]];
                            struct _host * hostB=&ipList[indexOrdered[d+1]];
                            if(average && hostA->resultsCount>=sizeof(hostA->results))
                                if (hostA->resultSuccess-3 > hostB->resultSuccess && hostA->resultSuccess*9/10>hostB->resultSuccess) /* For decreasing order use < */
                                {
                                    unsigned swap       = indexOrdered[d];
                                    indexOrdered[d]   = indexOrdered[d+1];
                                    indexOrdered[d+1] = swap;
                                }
                            //else if(priority) already sorted by priority
                        }
                    }
                }

                for (unsigned int z = 0; z < hostcount; z++)
                {
                    struct _host * host=&ipList[z];
                    if(!firstPing)
                    {
                        if(host->resultsCount>=sizeof(host->results))
                        {
                            if(host->results[0]==true)
                                host->resultSuccess--;//drop the first need decrease this counter
                            memcpy(host->results,host->results+1,sizeof(host->results)-1);
                            host->resultsCount--;
                        }
                        host->results[host->resultsCount]=host->replyReceived;
                        host->resultsCount++;
                        bool isFullyUp=false;
                        bool isFullyDown=false;
                        if(host->resultsCount>=8)
                        {
                            /*printf("[%s] %s [%i][%i][%i][%i][%i][%i][%i][%i] host->resultsCount\n", gettime(), host->address,
                            host->results[host->resultsCount-8],host->results[host->resultsCount-7],
                            host->results[host->resultsCount-6],host->results[host->resultsCount-5],
                            host->results[host->resultsCount-4],host->results[host->resultsCount-3],
                            host->results[host->resultsCount-2],host->results[host->resultsCount-1],
                            host->resultsCount
                            );*/
                            //printf("[%s] %s host->resultsCount>=8\n", gettime(), host->address);
                            isFullyUp=true;
                            isFullyDown=true;
                            unsigned int index=host->resultsCount-8;
                            while(index<host->resultsCount)
                            {
                                if(host->results[index]==false)
                                    isFullyUp=false;
                                else
                                    isFullyDown=false;
                                index++;
                            }
                        }
                        //printf("[%s] %s is now %i %i last %i\n", gettime(), host->address, isFullyUp, isFullyDown, host->replyReceived);
                        if(isFullyUp && host->lastState==false)
                        {
                            printf("[%s] %s is now UP\n", gettime(), host->address);
                            host->lastState=true;

                            struct stat sb;
                            if(stat("up-without-retry.sh",&sb)==0 && !noscript)
                            {
                                char * saveUp = malloc(strlen(argv[0])+strlen("up-without-retry.sh ")+strlen(host->address)+1);
                                strcpy(saveUp, argv[0]); /* copy name into the new var */
                                strcat(saveUp, "up-without-retry.sh "); /* copy name into the new var */
                                strcat(saveUp, host->address); /* add the extension */
                                system(saveUp);
                                free(saveUp);
                            }
                        }
                        if(isFullyDown && host->lastState==true)
                        {
                            printf("[%s] %s is now DOWN\n", gettime(), host->address);
                            host->lastState=false;

                            struct stat sb;
                            if(stat("down-without-retry.sh",&sb)==0 && !noscript)
                            {
                                //save the trace route
                                char * saveDown = malloc(strlen(argv[0])+strlen("down-without-retry.sh ")+strlen(host->address)+1);
                                strcpy(saveDown, argv[0]); /* copy name into the new var */
                                strcat(saveDown, "down-without-retry.sh "); /* copy name into the new var */
                                strcat(saveDown, host->address); /* add the extension */
                                system(saveDown);
                                free(saveDown);
                            }
                        }
                        if(host->lastState==true)
                            if(firstUpIP==-1)
                                firstUpIP=z;
                    }
                    host->replyReceived=false;
                    const int sd=host->sd;
                    struct sockaddr_in *saddr=ipList[z].socket;
                    ping(saddr, sd, seq);
                }
                bool changeGateway=false;
                if(priority || average)
                {
                    if(lastUpIP!=firstUpIP)
                        changeGateway=true;
                }
                else
                {
                    if(firstUpIP!=-1 && (lastUpIP==-1 || ipList[lastUpIP].lastState==false))
                        changeGateway=true;
                }
                if(changeGateway)
                {
                    callSkip=0;
                    lastUpIP=firstUpIP;
                    if(full!=NULL)
                    {
                        free(full);
                        struct stat sb;
                        if(stat("up.sh",&sb)==0 && !noscript)
                        {
                            full = malloc(strlen(argv[0])+strlen(scriptbase)+strlen(ipList[lastUpIP].address)+1);
                            strcpy(full, argv[0]); /* copy name into the new var */
                            strcat(full, scriptbase); /* copy name into the new var */
                            strcat(full, ipList[lastUpIP].address); /* add the extension */
                            printf("%s is now first valide ip route (call: %s later to previous command failed)\n", ipList[lastUpIP].address, full);
                        }
                        else
                            full = NULL;
                    }
                    else
                    {
                        struct stat sb;
                        if(stat("up.sh",&sb)==0 && !noscript)
                        {
                            full = malloc(strlen(argv[0])+strlen(scriptbase)+strlen(ipList[lastUpIP].address)+1);
                            strcpy(full, argv[0]); /* copy name into the new var */
                            strcat(full, scriptbase); /* copy name into the new var */
                            strcat(full, ipList[lastUpIP].address); /* add the extension */
                            printf("[%s] %s is now first valide ip route (call: %s)\n", gettime(), ipList[lastUpIP].address, full);
                            if(system(full)==0)
                            {
                                free(full);
                                full=NULL;
                            }
                            else
                                printf("call: %s failed, call later\n", full);
                        }
                    }
                }
                else if(full!=NULL)//recall
                {
                    callSkip++;
                    if(callSkip>15)
                    {
                        callSkip=0;
                        printf("[%s] re call: %s\n", gettime(), full);
                        if(system(full)==0)
                        {
                            free(full);
                            full=NULL;
                        }
                        else
                            printf("call: %s failed, call later\n", full);
                    }
                }
                firstPing=false;
            }
            if (events[n].data.fd == sdmain) {
                int bytes;
                unsigned int len = sizeof(addrmain);

                memset(buf, 0, sizeof(buf));
                bytes = recvfrom(sdmain, buf, sizeof(buf), 0, (struct sockaddr*) &addrmain, &len);
                if (bytes > 0)
                    parseReply(buf);
                else
                    perror("recvfrom");
            }
        }
    }

    return 0;
}
