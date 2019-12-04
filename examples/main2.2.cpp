#include <sys/socket.h>
#include <sys/errno.h>
#include <netdb.h>
#include <assert.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/event.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include "./taskruntime2.h"

void silk__readed( const int socket, char* buf, const int nbytes ) {
    if (nbytes == 0 || (nbytes < 0 && errno == ECONNRESET)) {
        close(socket);
    } else if (nbytes < 0) {
        printf("[%d] silk__readed(%d) error: %d\n", silk__current_worker_id, socket, errno);
        close(socket);
    } else {
        printf("[%d] silk__readed(%d) [%d] %s\n", silk__current_worker_id, socket, nbytes, buf);

        silk__read_async(socket, buf, 1024, silk__readed);
    }
}

int main() {
    silk__init_pool(silk__schedule, silk__makeuwcontext);

    struct addrinfo hints, *ser;

    memset(&hints, 0, sizeof hints);

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    getaddrinfo(NULL, "3491", & hints, &ser);

    int listensockfd = socket(ser->ai_family, ser->ai_socktype, ser->ai_protocol);

    fcntl(listensockfd, F_SETFL, fcntl(listensockfd, F_GETFL, 0) | O_NONBLOCK);

    int yes = 1;
    setsockopt(listensockfd, SOL_SOCKET, SO_REUSEPORT, & yes, sizeof(int));

    bind(listensockfd, ser-> ai_addr, ser-> ai_addrlen);

    listen(listensockfd, SOMAXCONN);

    kq = kqueue();
    struct kevent evSet;
    struct kevent evList[1024];

    EV_SET(&evSet, listensockfd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    assert(-1 != kevent(kq, & evSet, 1, NULL, 0, NULL));
    
    int n = 0;

    while (1) {
        int nev = kevent(kq, NULL, 0, evList, 1024, NULL); //io poll...

        for (int i = 0; i < nev; i++) {  //run pending...
            if (evList[i].ident == listensockfd) {
                while (1) {
                    struct sockaddr_storage addr;
                    socklen_t socklen = sizeof(addr);
                    
                    int clientsockfd = accept(evList[i].ident, (struct sockaddr *)&addr, &socklen);
                    if (clientsockfd == -1 && (errno == EAGAIN || errno == ECONNRESET)) {
                        break;
                    }

                    if (clientsockfd == -1) {
                        break;
                    }

                    fcntl(clientsockfd, F_SETFL, fcntl(clientsockfd, F_GETFL, 0) | O_NONBLOCK);

                    silk__read_async(clientsockfd, new char[1024], 1024, silk__readed);
                }
            }  else if (evList[i].filter == EVFILT_READ) {
                silk__io_read_frame* frame = (silk__io_read_frame*) evList[i].udata;

                memset(frame->buf, 0, frame->nbytes);

                n = evList[i].flags & EV_EOF ? 0 : read(evList[i].ident, frame->buf, frame->nbytes);

                frame->continuation->set_read_result(evList[i].ident, frame->buf, n);

                spawn(*frame->continuation);

                delete frame;
            }
        }
    }

    return 0;
}