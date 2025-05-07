#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <libgen.h>
#include <sys/select.h>
#include <time.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_DATAGRAM_SIZE 512
#define UDP_TIMEOUT_SEC 1
#define UDP_MAX_RETRIES 3

typedef enum { PROTO_TCP, PROTO_UDP } Protocol;

// UDP message header
typedef struct {
    uint32_t seq; // Numéro de séquence
    char type[5]; // Type de message (ex. LIST, WAIT) + terminateur nul
    uint32_t len; // Longueur de la charge utile
} UdpHeader;

int create_socket(Protocol proto) {
    int sockfd = socket(AF_INET, proto == PROTO_TCP ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (sockfd < 0) {
        fprintf(stderr, "./Mahdi: Problème lors de la création du socket : %s\n", strerror(errno));
        return -1;
    }
    return sockfd;
}

int send_udp_request(int sockfd, struct sockaddr_in *serv_addr, char *buffer, size_t len, char *response, size_t resp_size) {
    static uint32_t seq = 0;
    UdpHeader header = { seq++, "", (uint32_t)len };
    strncpy(header.type, strncmp(buffer, "LIST", 4) == 0 ? "LIST" : 
                        strncmp(buffer, "RESERVER", 8) == 0 ? "RSRV" : 
                        strncmp(buffer, "ANNULER", 7) == 0 ? "ANUL" : 
                        strncmp(buffer, "FACTURE", 7) == 0 ? "FACT" : "UNKN", 5);

    char packet[MAX_DATAGRAM_SIZE];
    memcpy(packet, &header, sizeof(UdpHeader));
    memcpy(packet + sizeof(UdpHeader), buffer, len);
    size_t packet_len = sizeof(UdpHeader) + len;

    struct timeval tv = { UDP_TIMEOUT_SEC, 0 };
    int retries = 0;
    ssize_t n;

    while (retries < UDP_MAX_RETRIES) {
        if (sendto(sockfd, packet, packet_len, 0, (struct sockaddr *)serv_addr, sizeof(*serv_addr)) < 0) {
            fprintf(stderr, "./Mahdi: Échec de l'envoi de la requête UDP : %s\n", strerror(errno));
            return -1;
        }

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        int ready = select(sockfd + 1, &readfds, NULL, NULL, &tv);
        if (ready < 0) {
            fprintf(stderr, "./Mahdi: Erreur dans la vérification des sockets : %s\n", strerror(errno));
            return -1;
        }
        if (ready == 0) {
            retries++;
            printf("./Mahdi: Aucune réponse, tentative %d sur %d\n", retries, UDP_MAX_RETRIES);
            continue;
        }

        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        n = recvfrom(sockfd, packet, MAX_DATAGRAM_SIZE, 0, (struct sockaddr *)&from_addr, &from_len);
        if (n < 0) {
            fprintf(stderr, "./Mahdi: Problème lors de la réception UDP : %s\n", strerror(errno));
            return -1;
        }

        if (n < sizeof(UdpHeader)) {
            printf("./Mahdi: Paquet reçu incomplet\n");
            continue;
        }

        UdpHeader recv_header;
        memcpy(&recv_header, packet, sizeof(UdpHeader));
        if (recv_header.seq != header.seq) {
            printf("./Mahdi: Séquence non correspondante, paquet ignoré\n");
            continue;
        }

        size_t payload_len = n - sizeof(UdpHeader);
        if (payload_len > resp_size - 1) {
            payload_len = resp_size - 1;
        }
        memcpy(response, packet + sizeof(UdpHeader), payload_len);
        response[payload_len] = '\0';

        if (strncmp(recv_header.type, "WAIT", 4) == 0) {
            printf("./Mahdi: %s\n", response);
            continue; // Attendre le prochain paquet
        }

        return payload_len;
    }

    printf("./Mahdi: Échec après %d tentatives\n", UDP_MAX_RETRIES);
    return -1;
}

int main(int argc, char *argv[]) {
    Protocol proto = PROTO_TCP;
    if (argc > 1) {
        if (strcmp(argv[1], "tcp") == 0) {
            proto = PROTO_TCP;
        } else if (strcmp(argv[1], "udp") == 0) {
            proto = PROTO_UDP;
        } else {
            fprintf(stderr, "./Mahdi: Protocole non reconnu (utilisez 'tcp' ou 'udp')\n");
            return 1;
        }
    }

    int sockfd = -1;
    struct sockaddr_in serv_addr;
    char buffer[BUFFER_SIZE];
    char agence[50];

    // Obtenir le nom de l'agence
    char *exec_name = basename(argv[0]);
    strncpy(agence, exec_name, sizeof(agence) - 1);
    agence[sizeof(agence) - 1] = '\0';
    if (strlen(agence) == 0) {
        fprintf(stderr, "./Mahdi: Nom d'agence invalide\n");
        return 1;
    }

    // Créer le socket
    sockfd = create_socket(proto);
    if (sockfd < 0) {
        return 1;
    }

    // Configurer l'adresse du serveur
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(PORT);
    if (inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr) <= 0) {
        fprintf(stderr, "./Mahdi: Adresse IP non valide : %s\n", strerror(errno));
        close(sockfd);
        return 1;
    }

    // Connexion pour TCP
    if (proto == PROTO_TCP) {
        if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
            fprintf(stderr, "./Mahdi: Impossible de se connecter au serveur : %s\n", strerror(errno));
            close(sockfd);
            return 1;
        }
    }

    int choix;
    while (1) {
        printf("\n./Mahdi: **** Gestion des Vols ****\n");
        printf("./Mahdi: 1. Afficher les vols disponibles\n");
        printf("./Mahdi: 2. Effectuer une réservation\n");
        printf("./Mahdi: 3. Supprimer une réservation\n");
        printf("./Mahdi: 4. Vérifier la facture\n");
        printf("./Mahdi: 0. Terminer\n");
        printf("./Mahdi: Saisissez une option : ");
        if (scanf("%d", &choix) != 1) {
            while (getchar() != '\n');
            printf("./Mahdi: Option non valide\n");
            continue;
        }
        while (getchar() != '\n');

        if (choix == 0) {
            printf("./Mahdi: Connexion terminée\n");
            break;
        }

        memset(buffer, 0, BUFFER_SIZE);

        switch (choix) {
            case 1: {
                strncpy(buffer, "LIST", 5);
                size_t len = strlen(buffer);
                if (proto == PROTO_TCP) {
                    if (write(sockfd, buffer, len) != len) {
                        fprintf(stderr, "./Mahdi: Erreur lors de l'envoi de la requête LIST : %s\n", strerror(errno));
                        close(sockfd);
                        return 1;
                    }
                    printf("./Mahdi: \nListe des vols :\n");
                    while (1) {
                        ssize_t n = read(sockfd, buffer, BUFFER_SIZE - 1);
                        if (n < 0) {
                            fprintf(stderr, "./Mahdi: Problème de lecture de la réponse LIST : %s\n", strerror(errno));
                            close(sockfd);
                            return 1;
                        }
                        if (n == 0) {
                            printf("./Mahdi: Serveur déconnecté\n");
                            close(sockfd);
                            return 1;
                        }
                        buffer[n] = '\0';
                        if (strncmp(buffer, "WAIT", 4) == 0) {
                            printf("./Mahdi: %s\n", buffer + 5);
                            continue;
                        }
                        printf("./Mahdi: %s", buffer);
                        if (strstr(buffer, "END\n") != NULL) {
                            break;
                        }
                    }
                } else { // UDP
                    printf("./Mahdi: \nListe des vols :\n");
                    ssize_t n = send_udp_request(sockfd, &serv_addr, buffer, len, buffer, BUFFER_SIZE);
                    if (n < 0) {
                        close(sockfd);
                        return 1;
                    }
                    while (strncmp(buffer, "END", 3) != 0) {
                        printf("./Mahdi: %s", buffer);
                        n = send_udp_request(sockfd, &serv_addr, "LIST", 4, buffer, BUFFER_SIZE);
                        if (n < 0) {
                            close(sockfd);
                            return 1;
                        }
                    }
                    printf("./Mahdi: END\n");
                }
                break;
            }

            case 2: {
                int ref, nb;
                printf("./Mahdi: Indiquez le numéro du vol : ");
                if (scanf("%d", &ref) != 1 || ref < 0) {
                    printf("./Mahdi: Numéro de vol incorrect\n");
                    while (getchar() != '\n');
                    continue;
                }
                printf("./Mahdi: Nombre de sièges à réserver : ");
                if (scanf("%d", &nb) != 1 || nb <= 0) {
                    printf("./Mahdi: Nombre de sièges non valide\n");
                    while (getchar() != '\n');
                    continue;
                }
                while (getchar() != '\n');
                snprintf(buffer, BUFFER_SIZE, "RESERVER %d %d %s", ref, nb, agence);
                size_t len = strlen(buffer);
                if (proto == PROTO_TCP) {
                    if (write(sockfd, buffer, len) != len) {
                        fprintf(stderr, "./Mahdi: Erreur lors de l'envoi de la réservation : %s\n", strerror(errno));
                        close(sockfd);
                        return 1;
                    }
                } else { // UDP
                    ssize_t n = send_udp_request(sockfd, &serv_addr, buffer, len, buffer, BUFFER_SIZE);
                    if (n < 0) {
                        close(sockfd);
                        return 1;
                    }
                }
                break;
            }

            case 3: {
                int ref, nb;
                printf("./Mahdi: Indiquez le numéro du vol à annuler : ");
                if (scanf("%d", &ref) != 1 || ref < 0) {
                    printf("./Mahdi: Numéro de vol incorrect\n");
                    while (getchar() != '\n');
                    continue;
                }
                printf("./Mahdi: Nombre de sièges à annuler : ");
                if (scanf("%d", &nb) != 1 || nb <= 0) {
                    printf("./Mahdi: Nombre de sièges non valide\n");
                    while (getchar() != '\n');
                    continue;
                }
                while (getchar() != '\n');
                snprintf(buffer, BUFFER_SIZE, "ANNULER %d %d %s", ref, nb, agence);
                size_t len = strlen(buffer);
                if (proto == PROTO_TCP) {
                    if (write(sockfd, buffer, len) != len) {
                        fprintf(stderr, "./Mahdi: Erreur lors de l'envoi de l'annulation : %s\n", strerror(errno));
                        close(sockfd);
                        return 1;
                    }
                } else { // UDP
                    ssize_t n = send_udp_request(sockfd, &serv_addr, buffer, len, buffer, BUFFER_SIZE);
                    if (n < 0) {
                        close(sockfd);
                        return 1;
                    }
                }
                break;
            }

            case 4: {
                snprintf(buffer, BUFFER_SIZE, "FACTURE %s", agence);
                size_t len = strlen(buffer);
                if (proto == PROTO_TCP) {
                    if (write(sockfd, buffer, len) != len) {
                        fprintf(stderr, "./Mahdi: Erreur lors de la demande de facture : %s\n", strerror(errno));
                        close(sockfd);
                        return 1;
                    }
                } else { // UDP
                    ssize_t n = send_udp_request(sockfd, &serv_addr, buffer, len, buffer, BUFFER_SIZE);
                    if (n < 0) {
                        close(sockfd);
                        return 1;
                    }
                }
                break;
            }

            default:
                printf("./Mahdi: Option non valide\n");
                continue;
        }

        if (choix != 1) {
            if (proto == PROTO_TCP) {
                while (1) {
                    ssize_t n = read(sockfd, buffer, BUFFER_SIZE - 1);
                    if (n < 0) {
                        fprintf(stderr, "./Mahdi: Problème lors de la lecture de la réponse : %s\n", strerror(errno));
                        close(sockfd);
                        return 1;
                    }
                    if (n == 0) {
                        printf("./Mahdi: Serveur déconnecté\n");
                        close(sockfd);
                        return 1;
                    }
                    buffer[n] = '\0';
                    if (strncmp(buffer, "WAIT", 4) == 0) {
                        printf("./Mahdi: %s\n", buffer + 5);
                        continue;
                    }
                    printf("./Mahdi: \nRésultat :\n%s\n", buffer);
                    break;
                }
            } else { // Réponse UDP déjà gérée dans send_udp_request
                printf("./Mahdi: \nRésultat :\n%s\n", buffer);
            }
        }
    }

    close(sockfd);
    printf("./Mahdi: Connexion terminée\n");
    return 0;
}
