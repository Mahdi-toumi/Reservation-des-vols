#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_DATAGRAM_SIZE 512
#define VOL_FILE "vols.txt"
#define HISTO_FILE "histo.txt"
#define FACTURE_FILE "facture.txt"

typedef enum { PROTO_TCP, PROTO_UDP } Protocol;

// Global mutexes for file access
pthread_mutex_t vols_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t histo_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t facture_mutex = PTHREAD_MUTEX_INITIALIZER;

// UDP message header
typedef struct {
    uint32_t seq; // Sequence number
    char type[5]; // Message type (e.g., LIST, WAIT) + null terminator
    uint32_t len; // Payload length
} UdpHeader;

// Print message with timestamp and Result prefix
void debug_print(const char *msg, const struct sockaddr_in *cli_addr, int sockfd) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", t);
    
    if (cli_addr) {
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli_addr->sin_addr, client_ip, INET_ADDRSTRLEN);
        printf("Result : %s (client %s:%d)\n" , msg, client_ip, ntohs(cli_addr->sin_port));
    } else if (sockfd >= 0) {
        printf("Result : %s (socket %d)\n", msg, sockfd);
    } else {
        printf("Result : %s\n", msg);
    }
}

int create_socket(Protocol proto) {
    int sockfd = socket(AF_INET, proto == PROTO_TCP ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "Result: Failed to create socket: %s\n", strerror(errno));
        return -1;
    }
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        fprintf(stderr, "Result: Failed to set socket options: %s\n", strerror(errno));
        close(sockfd);
        return -1;
    }
    debug_print("Socket created successfully", NULL, sockfd);
    return sockfd;
}

// Send waiting message to client
void send_wait_message(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, const char *resource, Protocol proto, uint32_t seq) {
    char msg[BUFFER_SIZE];
    snprintf(msg, sizeof(msg), "WAIT Waiting: another client is accessing %s", resource);
    debug_print("Sending wait message", cli_addr, sock);
    if (proto == PROTO_TCP) {
        if (write(sock, msg, strlen(msg)) < 0) {
            fprintf(stderr, "Result: Failed to send wait message: %s\n", strerror(errno));
        }
    } else {
        UdpHeader header = { seq, "WAIT", (uint32_t)strlen(msg) };
        char packet[MAX_DATAGRAM_SIZE];
        memcpy(packet, &header, sizeof(UdpHeader));
        memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
        if (sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len) < 0) {
            fprintf(stderr, "Result: Failed to send wait message via UDP: %s\n", strerror(errno));
        }
    }
}

void logHisto(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, int ref, const char *agence, const char *operation, int valeur, const char *resultat, Protocol proto, uint32_t seq) {
    char debug_msg[BUFFER_SIZE];
    snprintf(debug_msg, sizeof(debug_msg), "Logging history: ref=%d, agency=%s, op=%s, value=%d, result=%s", ref, agence, operation, valeur, resultat);
    debug_print(debug_msg, cli_addr, sock);
    
    if (pthread_mutex_trylock(&histo_mutex) != 0) {
        send_wait_message(sock, cli_addr, cli_len, "history file", proto, seq);
        pthread_mutex_lock(&histo_mutex);
    }
    FILE *f = fopen(HISTO_FILE, "a");
    if (!f) {
        fprintf(stderr, "Result: Failed to open history file: %s\n", strerror(errno));
        pthread_mutex_unlock(&histo_mutex);
        return;
    }
    fprintf(f, "%d %s %s %d %s\n", ref, agence, operation, valeur, resultat);
    fclose(f);
    debug_print("History logged successfully", cli_addr, sock);
    pthread_mutex_unlock(&histo_mutex);
}

void updateFacture(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, const char *agence, int montant, Protocol proto, uint32_t seq) {
    char debug_msg[BUFFER_SIZE];
    snprintf(debug_msg, sizeof(debug_msg), "Updating invoice for agency %s, amount=%d", agence, montant);
    debug_print(debug_msg, cli_addr, sock);
    
    if (pthread_mutex_trylock(&facture_mutex) != 0) {
        send_wait_message(sock, cli_addr, cli_len, "invoice file", proto, seq);
        pthread_mutex_lock(&facture_mutex);
    }
    FILE *f = fopen(FACTURE_FILE, "r");
    FILE *tmp = fopen("temp_facture.txt", "w");
    char line[BUFFER_SIZE];
    int found = 0;

    if (!f || !tmp) {
        if (f) fclose(f);
        if (tmp) fclose(tmp);
        fprintf(stderr, "Result: Failed to access invoice files: %s\n", strerror(errno));
        pthread_mutex_unlock(&facture_mutex);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        char ag[50];
        int somme;
        if (sscanf(line, "%s %d", ag, &somme) == 2) {
            if (strcmp(ag, agence) == 0) {
                somme += montant;
                found = 1;
            }
            fprintf(tmp, "%s %d\n", ag, somme);
        }
    }
    if (!found) {
        fprintf(tmp, "%s %d\n", agence, montant);
    }

    fclose(f);
    fclose(tmp);
    if (remove(FACTURE_FILE) != 0 || rename("temp_facture.txt", FACTURE_FILE) != 0) {
        fprintf(stderr, "Result: Failed to update invoice file: %s\n", strerror(errno));
    }
    debug_print("Invoice updated successfully", cli_addr, sock);
    pthread_mutex_unlock(&facture_mutex);
}

void sendVols(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, Protocol proto, uint32_t seq) {
    debug_print("Sending flight list", cli_addr, sock);
    if (pthread_mutex_trylock(&vols_mutex) != 0) {
        send_wait_message(sock, cli_addr, cli_len, "flight list", proto, seq);
        pthread_mutex_lock(&vols_mutex);
    }
    FILE *f = fopen(VOL_FILE, "r");
    if (!f) {
        char err[] = "Error: Unable to open flights file\n";
        debug_print("Failed to open flights file", cli_addr, sock);
        if (proto == PROTO_TCP) {
            write(sock, err, strlen(err));
        } else {
            UdpHeader header = { seq, "ERR", (uint32_t)strlen(err) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), err, strlen(err));
            sendto(sock, packet, sizeof(UdpHeader) + strlen(err), 0, (struct sockaddr *)cli_addr, cli_len);
        }
        pthread_mutex_unlock(&vols_mutex);
        return;
    }
    char line[BUFFER_SIZE];
    while (fgets(line, sizeof(line), f)) {
        if (proto == PROTO_TCP) {
            if (write(sock, line, strlen(line)) < 0) {
                fprintf(stderr, "Result: Failed to send flight line: %s\n", strerror(errno));
                fclose(f);
                pthread_mutex_unlock(&vols_mutex);
                return;
            }
        } else {
            UdpHeader header = { seq, "LIST", (uint32_t)strlen(line) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), line, strlen(line));
            if (sendto(sock, packet, sizeof(UdpHeader) + strlen(line), 0, (struct sockaddr *)cli_addr, cli_len) < 0) {
                fprintf(stderr, "Result: Failed to send flight line via UDP: %s\n", strerror(errno));
                fclose(f);
                pthread_mutex_unlock(&vols_mutex);
                return;
            }
        }
    }
    char end[] = "END\n";
    if (proto == PROTO_TCP) {
        if (write(sock, end, strlen(end)) != strlen(end)) {
            fprintf(stderr, "Result: Failed to send END marker: %s\n", strerror(errno));
        }
    } else {
        UdpHeader header = { seq, "END", (uint32_t)strlen(end) };
        char packet[MAX_DATAGRAM_SIZE];
        memcpy(packet, &header, sizeof(UdpHeader));
        memcpy(packet + sizeof(UdpHeader), end, strlen(end));
        if (sendto(sock, packet, sizeof(UdpHeader) + strlen(end), 0, (struct sockaddr *)cli_addr, cli_len) < 0) {
            fprintf(stderr, "Result: Failed to send END marker via UDP: %s\n", strerror(errno));
        }
    }
    debug_print("Flight list sent successfully", cli_addr, sock);
    fclose(f);
    pthread_mutex_unlock(&vols_mutex);
}

void reserverVol(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, int ref, int nb_places, const char *agence, Protocol proto, uint32_t seq) {
    char debug_msg[BUFFER_SIZE];
    snprintf(debug_msg, sizeof(debug_msg), "Processing reservation: ref=%d, seats=%d, agency=%s", ref, nb_places, agence);
    debug_print(debug_msg, cli_addr, sock);
    
    if (pthread_mutex_trylock(&vols_mutex) != 0) {
        send_wait_message(sock, cli_addr, cli_len, "flight list", proto, seq);
        pthread_mutex_lock(&vols_mutex);
    }
    FILE *f = fopen(VOL_FILE, "r");
    FILE *tmp = fopen("temp.txt", "w");
    char line[BUFFER_SIZE];
    int trouvé = 0;

    if (!f || !tmp) {
        char err[] = "Error: Unable to access flights file\n";
        debug_print("Failed to access flights file", cli_addr, sock);
        if (proto == PROTO_TCP) {
            write(sock, err, strlen(err));
        } else {
            UdpHeader header = { seq, "ERR", (uint32_t)strlen(err) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), err, strlen(err));
            sendto(sock, packet, sizeof(UdpHeader) + strlen(err), 0, (struct sockaddr *)cli_addr, cli_len);
        }
        if (f) fclose(f);
        if (tmp) fclose(tmp);
        pthread_mutex_unlock(&vols_mutex);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        int r, places, prix;
        char dest[50];
        if (sscanf(line, "%d %s %d %d", &r, dest, &places, &prix) == 4) {
            if (r == ref) {
                trouvé = 1;
                if (places >= nb_places) {
                    places -= nb_places;
                    fprintf(tmp, "%d %s %d %d\n", r, dest, places, prix);
                    char msg[BUFFER_SIZE];
                    snprintf(msg, sizeof(msg), "Reservation confirmed: %d seats on flight %d\n", nb_places, ref);
                    if (proto == PROTO_TCP) {
                        if (write(sock, msg, strlen(msg)) < 0) {
                            fprintf(stderr, "Result: Failed to send confirmation: %s\n", strerror(errno));
                        }
                    } else {
                        UdpHeader header = { seq, "RSRV", (uint32_t)strlen(msg) };
                        char packet[MAX_DATAGRAM_SIZE];
                        memcpy(packet, &header, sizeof(UdpHeader));
                        memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
                        sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len);
                    }
                    logHisto(sock, cli_addr, cli_len, ref, agence, "RESERVATION", nb_places, "OK", proto, seq);
                    updateFacture(sock, cli_addr, cli_len, agence, nb_places * prix, proto, seq);
                } else {
                    fprintf(tmp, "%s", line);
                    char msg[BUFFER_SIZE];
                    snprintf(msg, sizeof(msg), "Error: only %d seats available\n", places);
                    if (proto == PROTO_TCP) {
                        if (write(sock, msg, strlen(msg)) < 0) {
                            fprintf(stderr, "Result: Failed to send error message: %s\n", strerror(errno));
                        }
                    } else {
                        UdpHeader header = { seq, "ERR", (uint32_t)strlen(msg) };
                        char packet[MAX_DATAGRAM_SIZE];
                        memcpy(packet, &header, sizeof(UdpHeader));
                        memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
                        sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len);
                    }
                    logHisto(sock, cli_addr, cli_len, ref, agence, "RESERVATION", nb_places, "FAILED", proto, seq);
                }
            } else {
                fprintf(tmp, "%s", line);
            }
        } else {
            fprintf(tmp, "%s", line);
        }
    }

    fclose(f);
    fclose(tmp);
    if (!trouvé) {
        char msg[] = "Error: Flight reference not found\n";
        debug_print("Flight reference not found", cli_addr, sock);
        if (proto == PROTO_TCP) {
            if (write(sock, msg, strlen(msg)) < 0) {
                fprintf(stderr, "Result: Failed to send error message: %s\n", strerror(errno));
            }
        } else {
            UdpHeader header = { seq, "ERR", (uint32_t)strlen(msg) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
            sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len);
        }
        remove("temp.txt");
        logHisto(sock, cli_addr, cli_len, ref, agence, "RESERVATION", nb_places, "UNKNOWN", proto, seq);
    } else {
        if (remove(VOL_FILE) != 0 || rename("temp.txt", VOL_FILE) != 0) {
            fprintf(stderr, "Result: Failed to update flights file: %s\n", strerror(errno));
        }
        debug_print("Flights file updated successfully", cli_addr, sock);
    }
    pthread_mutex_unlock(&vols_mutex);
}

void annulerVol(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, int ref, int nb_places, const char *agence, Protocol proto, uint32_t seq) {
    char debug_msg[BUFFER_SIZE];
    snprintf(debug_msg, sizeof(debug_msg), "Processing cancellation: ref=%d, seats=%d, agency=%s", ref, nb_places, agence);
    debug_print(debug_msg, cli_addr, sock);
    
    if (pthread_mutex_trylock(&vols_mutex) != 0) {
        send_wait_message(sock, cli_addr, cli_len, "flight list", proto, seq);
        pthread_mutex_lock(&vols_mutex);
    }
    FILE *f = fopen(VOL_FILE, "r");
    FILE *tmp = fopen("temp.txt", "w");
    char line[BUFFER_SIZE];
    int trouvé = 0;

    if (!f || !tmp) {
        char err[] = "Error: Unable to access flights file\n";
        debug_print("Failed to access flights file", cli_addr, sock);
        if (proto == PROTO_TCP) {
            write(sock, err, strlen(err));
        } else {
            UdpHeader header = { seq, "ERR", (uint32_t)strlen(err) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), err, strlen(err));
            sendto(sock, packet, sizeof(UdpHeader) + strlen(err), 0, (struct sockaddr *)cli_addr, cli_len);
        }
        if (f) fclose(f);
        if (tmp) fclose(tmp);
        pthread_mutex_unlock(&vols_mutex);
        return;
    }

    while (fgets(line, sizeof(line), f)) {
        int r, places, prix;
        char dest[50];
        if (sscanf(line, "%d %s %d %d", &r, dest, &places, &prix) == 4) {
            if (r == ref) {
                trouvé = 1;
                places += nb_places;
                fprintf(tmp, "%d %s %d %d\n", r, dest, places, prix);
                int penalite = (int)(nb_places * prix * 0.1);
                updateFacture(sock, cli_addr, cli_len, agence, penalite, proto, seq);
                char msg[BUFFER_SIZE];
                snprintf(msg, sizeof(msg), "Cancellation confirmed: %d seats on flight %d (penalty %d€)\n", nb_places, ref, penalite);
                if (proto == PROTO_TCP) {
                    if (write(sock, msg, strlen(msg)) < 0) {
                        fprintf(stderr, "Result: Failed to send cancellation confirmation: %s\n", strerror(errno));
                    }
                } else {
                    UdpHeader header = { seq, "ANUL", (uint32_t)strlen(msg) };
                    char packet[MAX_DATAGRAM_SIZE];
                    memcpy(packet, &header, sizeof(UdpHeader));
                    memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
                    sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len);
                }
                logHisto(sock, cli_addr, cli_len, ref, agence, "CANCELLATION", nb_places, "OK", proto, seq);
            } else {
                fprintf(tmp, "%s", line);
            }
        } else {
            fprintf(tmp, "%s", line);
        }
    }

    fclose(f);
    fclose(tmp);
    if (!trouvé) {
        char msg[] = "Error: Flight reference not found\n";
        debug_print("Flight reference not found", cli_addr, sock);
        if (proto == PROTO_TCP) {
            if (write(sock, msg, strlen(msg)) < 0) {
                fprintf(stderr, "Result: Failed to send error message: %s\n", strerror(errno));
            }
        } else {
            UdpHeader header = { seq, "ERR", (uint32_t)strlen(msg) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
            sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len);
        }
        remove("temp.txt");
        logHisto(sock, cli_addr, cli_len, ref, agence, "CANCELLATION", nb_places, "UNKNOWN", proto, seq);
    } else {
        if (remove(VOL_FILE) != 0 || rename("temp.txt", VOL_FILE) != 0) {
            fprintf(stderr, "Result: Failed to update flights file: %s\n", strerror(errno));
        }
        debug_print("Flights file updated successfully", cli_addr, sock);
    }
    pthread_mutex_unlock(&vols_mutex);
}

void consulterFacture(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, const char *agence, Protocol proto, uint32_t seq) {
    char debug_msg[BUFFER_SIZE];
    snprintf(debug_msg, sizeof(debug_msg), "Fetching invoice for agency %s", agence);
    debug_print(debug_msg, cli_addr, sock);
    
    if (pthread_mutex_trylock(&facture_mutex) != 0) {
        send_wait_message(sock, cli_addr, cli_len, "invoice file", proto, seq);
        pthread_mutex_lock(&facture_mutex);
    }
    FILE *f = fopen(FACTURE_FILE, "r");
    int found = 0;
    if (f) {
        char line[BUFFER_SIZE];
        while (fgets(line, sizeof(line), f)) {
            char ag[50];
            int montant;
            if (sscanf(line, "%s %d", ag, &montant) == 2 && strcmp(ag, agence) == 0) {
                char msg[BUFFER_SIZE];
                snprintf(msg, sizeof(msg), "Invoice for %s: %d€\n", agence, montant);
                if (proto == PROTO_TCP) {
                    if (write(sock, msg, strlen(msg)) < 0) {
                        fprintf(stderr, "Result: Failed to send invoice: %s\n", strerror(errno));
                    }
                } else {
                    UdpHeader header = { seq, "FACT", (uint32_t)strlen(msg) };
                    char packet[MAX_DATAGRAM_SIZE];
                    memcpy(packet, &header, sizeof(UdpHeader));
                    memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
                    sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len);
                }
                found = 1;
                break;
            }
        }
        fclose(f);
    }
    if (!found) {
        char msg[] = "No invoice found for this agency\n";
        debug_print("No invoice found", cli_addr, sock);
        if (proto == PROTO_TCP) {
            if (write(sock, msg, strlen(msg)) < 0) {
                fprintf(stderr, "Result: Failed to send no-invoice message: %s\n", strerror(errno));
            }
        } else {
            UdpHeader header = { seq, "ERR", (uint32_t)strlen(msg) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
            sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len);
        }
    }
    debug_print("Invoice request processed", cli_addr, sock);
    pthread_mutex_unlock(&facture_mutex);
}

// Thread function for TCP clients
void *handle_tcp_client(void *arg) {
    int newsockfd = *(int *)arg;
    free(arg);
    char buffer[BUFFER_SIZE];
    
    debug_print("New TCP client thread started", NULL, newsockfd);

    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        ssize_t n = read(newsockfd, buffer, BUFFER_SIZE - 1);
        if (n < 0) {
            fprintf(stderr, "Result: Error reading from client: %s\n", strerror(errno));
            break;
        }
        if (n == 0) {
            debug_print("Client disconnected", NULL, newsockfd);
            break;
        }
        buffer[n] = '\0';
        char debug_msg[BUFFER_SIZE];
        snprintf(debug_msg, sizeof(debug_msg), "Received command: %s", buffer);
        debug_print(debug_msg, NULL, newsockfd);

        if (strncmp(buffer, "LIST", 4) == 0) {
            sendVols(newsockfd, NULL, 0, PROTO_TCP, 0);
        } else if (strncmp(buffer, "RESERVER", 8) == 0) {
            int ref, nb;
            char agence[50];
            if (sscanf(buffer + 9, "%d %d %s", &ref, &nb, agence) == 3) {
                reserverVol(newsockfd, NULL, 0, ref, nb, agence, PROTO_TCP, 0);
            } else {
                char err[] = "Invalid RESERVER command\n";
                write(newsockfd, err, strlen(err));
                debug_print("Invalid RESERVER command", NULL, newsockfd);
            }
        } else if (strncmp(buffer, "ANNULER", 7) == 0) {
            int ref, nb;
            char agence[50];
            if (sscanf(buffer + 8, "%d %d %s", &ref, &nb, agence) == 3) {
                annulerVol(newsockfd, NULL, 0, ref, nb, agence, PROTO_TCP, 0);
            } else {
                char err[] = "Invalid ANNULER command\n";
                write(newsockfd, err, strlen(err));
                debug_print("Invalid ANNULER command", NULL, newsockfd);
            }
        } else if (strncmp(buffer, "FACTURE", 7) == 0) {
            char ag[50];
            if (sscanf(buffer + 8, "%s", ag) == 1) {
                consulterFacture(newsockfd, NULL, 0, ag, PROTO_TCP, 0);
            } else {
                char err[] = "Invalid FACTURE command\n";
                write(newsockfd, err, strlen(err));
                debug_print("Invalid FACTURE command", NULL, newsockfd);
            }
        } else {
            char err[] = "Unknown command\n";
            write(newsockfd, err, strlen(err));
            debug_print("Unknown command received", NULL, newsockfd);
        }
    }

    close(newsockfd);
    debug_print("TCP client thread terminated", NULL, newsockfd);
    return NULL;
}

void handle_udp_request(int sockfd, char *buffer, ssize_t n, struct sockaddr_in *cli_addr, socklen_t cli_len) {
    if (n < sizeof(UdpHeader)) {
        char err[] = "Datagram too short\n";
        UdpHeader header = { 0, "ERR", (uint32_t)strlen(err) };
        char packet[MAX_DATAGRAM_SIZE];
        memcpy(packet, &header, sizeof(UdpHeader));
        memcpy(packet + sizeof(UdpHeader), err, strlen(err));
        sendto(sockfd, packet, sizeof(UdpHeader) + strlen(err), 0, (struct sockaddr *)cli_addr, cli_len);
        debug_print("Received invalid datagram: too short", cli_addr, sockfd);
        return;
    }

    UdpHeader header;
    memcpy(&header, buffer, sizeof(UdpHeader));
    char *payload = buffer + sizeof(UdpHeader);
    payload[header.len] = '\0';

    char debug_msg[BUFFER_SIZE];
    snprintf(debug_msg, sizeof(debug_msg), "Received UDP command: %s", payload);
    debug_print(debug_msg, cli_addr, sockfd);

    if (strncmp(payload, "LIST", 4) == 0) {
        sendVols(sockfd, cli_addr, cli_len, PROTO_UDP, header.seq);
    } else if (strncmp(payload, "RESERVER", 8) == 0) {
        int ref, nb;
        char agence[50];
        if (sscanf(payload + 9, "%d %d %s", &ref, &nb, agence) == 3) {
            reserverVol(sockfd, cli_addr, cli_len, ref, nb, agence, PROTO_UDP, header.seq);
        } else {
            char err[] = "Invalid RESERVER command\n";
            UdpHeader header_out = { header.seq, "ERR", (uint32_t)strlen(err) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header_out, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), err, strlen(err));
            sendto(sockfd, packet, sizeof(UdpHeader) + strlen(err), 0, (struct sockaddr *)cli_addr, cli_len);
            debug_print("Invalid RESERVER command", cli_addr, sockfd);
        }
    } else if (strncmp(payload, "ANNULER", 7) == 0) {
        int ref, nb;
        char agence[50];
        if (sscanf(payload + 8, "%d %d %s", &ref, &nb, agence) == 3) {
            annulerVol(sockfd, cli_addr, cli_len, ref, nb, agence, PROTO_UDP, header.seq);
        } else {
            char err[] = "Invalid ANNULER command\n";
            UdpHeader header_out = { header.seq, "ERR", (uint32_t)strlen(err) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header_out, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), err, strlen(err));
            sendto(sockfd, packet, sizeof(UdpHeader) + strlen(err), 0, (struct sockaddr *)cli_addr, cli_len);
            debug_print("Invalid ANNULER command", cli_addr, sockfd);
        }
    } else if (strncmp(payload, "FACTURE", 7) == 0) {
        char ag[50];
        if (sscanf(payload + 8, "%s", ag) == 1) {
            consulterFacture(sockfd, cli_addr, cli_len, ag, PROTO_UDP, header.seq);
        } else {
            char err[] = "Invalid FACTURE command\n";
            UdpHeader header_out = { header.seq, "ERR", (uint32_t)strlen(err) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header_out, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), err, strlen(err));
            sendto(sockfd, packet, sizeof(UdpHeader) + strlen(err), 0, (struct sockaddr *)cli_addr, cli_len);
            debug_print("Invalid FACTURE command", cli_addr, sockfd);
        }
    } else {
        char err[] = "Unknown command\n";
        UdpHeader header_out = { header.seq, "ERR", (uint32_t)strlen(err) };
        char packet[MAX_DATAGRAM_SIZE];
        memcpy(packet, &header_out, sizeof(UdpHeader));
        memcpy(packet + sizeof(UdpHeader), err, strlen(err));
        sendto(sockfd, packet, sizeof(UdpHeader) + strlen(err), 0, (struct sockaddr *)cli_addr, cli_len);
        debug_print("Unknown command received", cli_addr, sockfd);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2 || (strcmp(argv[1], "tcp") != 0 && strcmp(argv[1], "udp") != 0)) {
        fprintf(stderr, "Result: Usage: %s <tcp|udp>\n", argv[0]);
        return 1;
    }
    Protocol proto = strcmp(argv[1], "tcp") == 0 ? PROTO_TCP : PROTO_UDP;

    int sockfd = -1;
    struct sockaddr_in serv_addr, cli_addr;
    socklen_t clilen = sizeof(cli_addr);

    // Create socket
    sockfd = create_socket(proto);
    if (sockfd < 0) {
        return 1;
    }

    // Configure server address
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT);

    // Bind socket
    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        fprintf(stderr, "Result: Failed to bind socket: %s\n", strerror(errno));
        close(sockfd);
        return 1;
    }
    debug_print("Socket bound successfully", NULL, sockfd);

    if (proto == PROTO_TCP) {
        // Listen for connections
        if (listen(sockfd, 5) < 0) {
            fprintf(stderr, "Result: Failed to listen on socket: %s\n", strerror(errno));
            close(sockfd);
            return 1;
        }
        debug_print("TCP server started", NULL, sockfd);

        while (1) {
            int *newsockfd = malloc(sizeof(int));
            if (!newsockfd) {
                fprintf(stderr, "Result: Failed to allocate memory for client socket: %s\n", strerror(errno));
                continue;
            }
            *newsockfd = accept(sockfd, (struct sockaddr *)&cli_addr, &clilen);
            if (*newsockfd < 0) {
                fprintf(stderr, "Result: Failed to accept client connection: %s\n", strerror(errno));
                free(newsockfd);
                continue;
            }
            char debug_msg[BUFFER_SIZE];
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cli_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
            snprintf(debug_msg, sizeof(debug_msg), "New client connected: %s:%d", client_ip, ntohs(cli_addr.sin_port));
            debug_print(debug_msg, NULL, *newsockfd);

            // Create a new thread for the client
            pthread_t thread;
            if (pthread_create(&thread, NULL, handle_tcp_client, newsockfd) != 0) {
                fprintf(stderr, "Result: Failed to create client thread: %s\n", strerror(errno));
                close(*newsockfd);
                free(newsockfd);
                continue;
            }

            // Detach the thread to avoid memory leaks
            if (pthread_detach(thread) != 0) {
                fprintf(stderr, "Result: Failed to detach client thread: %s\n", strerror(errno));
            }
        }
    } else {
        debug_print("UDP server started", NULL, sockfd);
        char buffer[MAX_DATAGRAM_SIZE];

        while (1) {
            memset(buffer, 0, MAX_DATAGRAM_SIZE);
            ssize_t n = recvfrom(sockfd, buffer, MAX_DATAGRAM_SIZE, 0, (struct sockaddr *)&cli_addr, &clilen);
            if (n < 0) {
                fprintf(stderr, "Result: Failed to receive UDP packet: %s\n", strerror(errno));
                continue;
            }
            handle_udp_request(sockfd, buffer, n, &cli_addr, clilen);
        }
    }

    close(sockfd);
    debug_print("Server socket closed", NULL, sockfd);
    pthread_mutex_destroy(&vols_mutex);
    pthread_mutex_destroy(&histo_mutex);
    pthread_mutex_destroy(&facture_mutex);
    return 0;
}
