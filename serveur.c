#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>

#define PORT 8080
#define BUFFER_SIZE 1024
#define MAX_DATAGRAM_SIZE 512
#define VOL_FILE "vols.txt"
#define HISTO_FILE "histo.txt"
#define FACTURE_FILE "facture.txt"

typedef enum { PROTO_TCP, PROTO_UDP } Protocol;

// Mutex globaux pour l'accès aux fichiers
pthread_mutex_t vols_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t histo_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t facture_mutex = PTHREAD_MUTEX_INITIALIZER;

// En-tête des messages UDP
typedef struct {
    uint32_t seq; // Numéro de séquence
    char type[5]; // Type de message (ex. LIST, WAIT) + terminateur nul
    uint32_t len; // Longueur de la charge utile
} UdpHeader;

// Afficher un message de débogage sans horodatage
void debug_print(const char *msg, const struct sockaddr_in *cli_addr, int sockfd) {
    if (cli_addr) {
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &cli_addr->sin_addr, client_ip, INET_ADDRSTRLEN);
        printf("RESULT : %s (client %s:%d)\n", msg, client_ip, ntohs(cli_addr->sin_port));
    } else if (sockfd >= 0) {
        printf("RESULT : %s (socket %d)\n", msg, sockfd);
    } else {
        printf("RESULT : %s\n", msg);
    }
}

int create_socket(Protocol proto) {
    int sockfd = socket(AF_INET, proto == PROTO_TCP ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("Échec de l'initialisation du socket");
        return -1;
    }
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Problème de configuration du socket");
        close(sockfd);
        return -1;
    }
    debug_print("Socket initialisé", NULL, sockfd);
    return sockfd;
}

// Envoyer un message d'attente au client
void send_wait_message(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, const char *resource, Protocol proto, uint32_t seq) {
    char msg[BUFFER_SIZE];
    snprintf(msg, sizeof(msg), "WAIT Veuillez patienter : accès concurrent aux %s", resource);
    debug_print("Envoi d'un message d'attente", cli_addr, sock);
    if (proto == PROTO_TCP) {
        if (write(sock, msg, strlen(msg)) < 0) {
            perror("Erreur lors de l'envoi du message d'attente");
        }
    } else {
        UdpHeader header = { seq, "WAIT", (uint32_t)strlen(msg) };
        char packet[MAX_DATAGRAM_SIZE];
        memcpy(packet, &header, sizeof(UdpHeader));
        memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
        if (sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len) < 0) {
            perror("Problème d'envoi du message d'attente UDP");
        }
    }
}

void logHisto(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, int ref, const char *agence, const char *operation, int valeur, const char *resultat, Protocol proto, uint32_t seq) {
    char debug_msg[BUFFER_SIZE];
    snprintf(debug_msg, sizeof(debug_msg), "Journalisation : vol=%d, agence=%s, action=%s, quantité=%d, statut=%s", ref, agence, operation, valeur, resultat);
    debug_print(debug_msg, cli_addr, sock);
    
    if (pthread_mutex_trylock(&histo_mutex) != 0) {
        send_wait_message(sock, cli_addr, cli_len, "données historiques", proto, seq);
        pthread_mutex_lock(&histo_mutex);
    }
    FILE *f = fopen(HISTO_FILE, "a");
    if (!f) {
        perror("Problème d'accès au fichier historique");
        pthread_mutex_unlock(&histo_mutex);
        return;
    }
    fprintf(f, "%d %s %s %d %s\n", ref, agence, operation, valeur, resultat);
    fclose(f);
    debug_print("Journalisation effectuée", cli_addr, sock);
    pthread_mutex_unlock(&histo_mutex);
}

void updateFacture(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, const char *agence, int montant, Protocol proto, uint32_t seq) {
    char debug_msg[BUFFER_SIZE];
    snprintf(debug_msg, sizeof(debug_msg), "Mise à jour du solde pour %s, montant=%d", agence, montant);
    debug_print(debug_msg, cli_addr, sock);
    
    if (pthread_mutex_trylock(&facture_mutex) != 0) {
        send_wait_message(sock, cli_addr, cli_len, "données de facturation", proto, seq);
        pthread_mutex_lock(&facture_mutex);
    }
    FILE *f = fopen(FACTURE_FILE, "r");
    FILE *tmp = fopen("temp_facture.txt", "w");
    char line[BUFFER_SIZE];
    int found = 0;

    if (!f || !tmp) {
        if (f) fclose(f);
        if (tmp) fclose(tmp);
        perror("Échec d'accès aux fichiers de facturation");
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
        perror("Problème lors de la mise à jour de la facturation");
    }
    debug_print("Solde mis à jour", cli_addr, sock);
    pthread_mutex_unlock(&facture_mutex);
}

void sendVols(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, Protocol proto, uint32_t seq) {
    debug_print("Transmission de la liste des vols", cli_addr, sock);
    if (pthread_mutex_trylock(&vols_mutex) != 0) {
        send_wait_message(sock, cli_addr, cli_len, "données des vols", proto, seq);
        pthread_mutex_lock(&vols_mutex);
    }
    FILE *f = fopen(VOL_FILE, "r");
    if (!f) {
        char err[] = "Problème : échec de l'accès au fichier des vols\n";
        debug_print("Impossible d'ouvrir le fichier des vols", cli_addr, sock);
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
                perror("Erreur lors de l'envoi d'une entrée de vol");
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
                perror("Problème d'envoi d'une entrée de vol UDP");
                fclose(f);
                pthread_mutex_unlock(&vols_mutex);
                return;
            }
        }
    }
    char end[] = "END\n";
    if (proto == PROTO_TCP) {
        if (write(sock, end, strlen(end)) != strlen(end)) {
            perror("Erreur lors de l'envoi du marqueur de fin");
        }
    } else {
        UdpHeader header = { seq, "END", (uint32_t)strlen(end) };
        char packet[MAX_DATAGRAM_SIZE];
        memcpy(packet, &header, sizeof(UdpHeader));
        memcpy(packet + sizeof(UdpHeader), end, strlen(end));
        if (sendto(sock, packet, sizeof(UdpHeader) + strlen(end), 0, (struct sockaddr *)cli_addr, cli_len) < 0) {
            perror("Problème d'envoi du marqueur de fin UDP");
        }
    }
    debug_print("Liste des vols transmise", cli_addr, sock);
    fclose(f);
    pthread_mutex_unlock(&vols_mutex);
}

void reserverVol(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, int ref, int nb_places, const char *agence, Protocol proto, uint32_t seq) {
    char debug_msg[BUFFER_SIZE];
    snprintf(debug_msg, sizeof(debug_msg), "Exécution de la réservation : vol=%d, sièges=%d, agence=%s", ref, nb_places, agence);
    debug_print(debug_msg, cli_addr, sock);
    
    if (pthread_mutex_trylock(&vols_mutex) != 0) {
        send_wait_message(sock, cli_addr, cli_len, "données des vols", proto, seq);
        pthread_mutex_lock(&vols_mutex);
    }
    FILE *f = fopen(VOL_FILE, "r");
    FILE *tmp = fopen("temp.txt", "w");
    char line[BUFFER_SIZE];
    int trouvé = 0;

    if (!f || !tmp) {
        char err[] = "Problème : échec de l'accès au fichier des vols\n";
        debug_print("Impossible d'accéder au fichier des vols", cli_addr, sock);
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
                    snprintf(msg, sizeof(msg), "Opération réussie : %d sièges affectés au vol %d\n", nb_places, ref);
                    if (proto == PROTO_TCP) {
                        if (write(sock, msg, strlen(msg)) < 0) {
                            perror("Erreur lors de l'envoi de la confirmation");
                        }
                    } else {
                        UdpHeader header = { seq, "RSRV", (uint32_t)strlen(msg) };
                        char packet[MAX_DATAGRAM_SIZE];
                        memcpy(packet, &header, sizeof(UdpHeader));
                        memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
                        sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len);
                    }
                    logHisto(sock, cli_addr, cli_len, ref, agence, "RÉSERVATION", nb_places, "OK", proto, seq);
                    updateFacture(sock, cli_addr, cli_len, agence, nb_places * prix, proto, seq);
                } else {
                    fprintf(tmp, "%s", line);
                    char msg[BUFFER_SIZE];
                    snprintf(msg, sizeof(msg), "Erreur : seulement %d sièges disponibles\n", places);
                    if (proto == PROTO_TCP) {
                        if (write(sock, msg, strlen(msg)) < 0) {
                            perror("Erreur lors de l'envoi du message d'erreur");
                        }
                    } else {
                        UdpHeader header = { seq, "ERR", (uint32_t)strlen(msg) };
                        char packet[MAX_DATAGRAM_SIZE];
                        memcpy(packet, &header, sizeof(UdpHeader));
                        memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
                        sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len);
                    }
                    logHisto(sock, cli_addr, cli_len, ref, agence, "RÉSERVATION", nb_places, "ÉCHEC", proto, seq);
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
        char msg[] = "Erreur : vol non trouvé\n";
        debug_print("Vol non référencé", cli_addr, sock);
        if (proto == PROTO_TCP) {
            if (write(sock, msg, strlen(msg)) < 0) {
                perror("Erreur lors de l'envoi du message d'erreur");
            }
        } else {
            UdpHeader header = { seq, "ERR", (uint32_t)strlen(msg) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
            sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len);
        }
        remove("temp.txt");
        logHisto(sock, cli_addr, cli_len, ref, agence, "RÉSERVATION", nb_places, "INCONNU", proto, seq);
    } else {
        if (remove(VOL_FILE) != 0 || rename("temp.txt", VOL_FILE) != 0) {
            perror("Problème lors de la mise à jour des vols");
        }
        debug_print("Mise à jour des vols effectuée", cli_addr, sock);
    }
    pthread_mutex_unlock(&vols_mutex);
}

void annulerVol(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, int ref, int nb_places, const char *agence, Protocol proto, uint32_t seq) {
    char debug_msg[BUFFER_SIZE];
    snprintf(debug_msg, sizeof(debug_msg), "Exécution de l'annulation : vol=%d, sièges=%d, agence=%s", ref, nb_places, agence);
    debug_print(debug_msg, cli_addr, sock);
    
    if (pthread_mutex_trylock(&vols_mutex) !=liaison pour éviter tout blocage
    send_wait_message(sock, cli_addr, cli_len, "données des vols", proto, seq);
    pthread_mutex_lock(&vols_mutex);
    FILE *f = fopen(VOL_FILE, "r");
    FILE *tmp = fopen("temp.txt", "w");
    char line[BUFFER_SIZE];
    int trouvé = 0;

    if (!f || !tmp) {
        char err[] = "Problème : échec de l'accès au fichier des vols\n";
        debug_print("Impossible d'accéder au fichier des vols", cli_addr, sock);
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
                snprintf(msg, sizeof(msg), "Opération réussie : %d sièges annulés pour le vol %d (pénalité %d€)\n", nb_places, ref, penalite);
                if (proto == PROTO_TCP) {
                    if (write(sock, msg, strlen(msg)) < 0) {
                        perror("Erreur lors de l'envoi de la confirmation");
                    }
                } else {
                    UdpHeader header = { seq, "ANUL", (uint32_t)strlen(msg) };
                    char packet[MAX_DATAGRAM_SIZE];
                    memcpy(packet, &header, sizeof(UdpHeader));
                    memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
                    sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len);
                }
                logHisto(sock, cli_addr, cli_len, ref, agence, "ANNULATION", nb_places, "OK", proto, seq);
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
        char msg[] = "Erreur : vol non trouvé\n";
        debug_print("Vol non référencé", cli_addr, sock);
        if (proto == PROTO_TCP) {
            if (write(sock, msg, strlen(msg)) < 0) {
                perror("Erreur lors de l'envoi du message d'erreur");
            }
        } else {
            UdpHeader header = { seq, "ERR", (uint32_t)strlen(msg) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
            sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len);
        }
        remove("temp.txt");
        logHisto(sock, cli_addr, cli_len, ref, agence, "ANNULATION", nb_places, "INCONNU", proto, seq);
    } else {
        if (remove(VOL_FILE) != 0 || rename("temp.txt", VOL_FILE) != 0) {
            perror("Problème lors de la mise à jour des vols");
        }
        debug_print("Mise à jour des vols effectuée", cli_addr, sock);
    }
    pthread_mutex_unlock(&vols_mutex);
}

void consulterFacture(int sock, struct sockaddr_in *cli_addr, socklen_t cli_len, const char *agence, Protocol proto, uint32_t seq) {
    char debug_msg[BUFFER_SIZE];
    snprintf(debug_msg, sizeof(debug_msg), "Consultation du solde pour %s", agence);
    debug_print(debug_msg, cli_addr, sock);
    
    if (pthread_mutex_trylock(&facture_mutex) != 0) {
        send_wait_message(sock, cli_addr, cli_len, "données de facturation", proto, seq);
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
                snprintf(msg, sizeof(msg), "Solde pour %s : %d€\n", agence, montant);
                if (proto == PROTO_TCP) {
                    if (write(sock, msg, strlen(msg)) < 0) {
                        perror("Erreur lors de l'envoi du solde");
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
        char msg[] = "Aucun solde enregistré pour cette agence\n";
        debug_print("Aucun solde trouvé", cli_addr, sock);
        if (proto == PROTO_TCP) {
            if (write(sock, msg, strlen(msg)) < 0) {
                perror("Erreur lors de l'envoi du message");
            }
        } else {
            UdpHeader header = { seq, "ERR", (uint32_t)strlen(msg) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), msg, strlen(msg));
            sendto(sock, packet, sizeof(UdpHeader) + strlen(msg), 0, (struct sockaddr *)cli_addr, cli_len);
        }
    }
    debug_print("Requête de solde traitée", cli_addr, sock);
    pthread_mutex_unlock(&facture_mutex);
}

// Fonction de thread pour les clients TCP
void *handle_tcp_client(void *arg) {
    int newsockfd = *(int *)arg;
    free(arg);
    char buffer[BUFFER_SIZE];
    
    debug_print("Nouveau client TCP connecté", NULL, newsockfd);

    while (1) {
        memset(buffer, 0, BUFFER_SIZE);
        ssize_t n = read(newsockfd, buffer, BUFFER_SIZE - 1);
        if (n < 0) {
            perror("Problème de lecture du client");
            break;
        }
        if (n == 0) {
            debug_print("Client déconnecté", NULL, newsockfd);
            break;
        }
        buffer[n] = '\0';
        char debug_msg[BUFFER_SIZE];
        snprintf(debug_msg, sizeof(debug_msg), "Requête traitée : %s", buffer);
        debug_print(debug_msg, NULL, newsockfd);

        if (strncmp(buffer, "LIST", 4) == 0) {
            sendVols(newsockfd, NULL, 0, PROTO_TCP, 0);
        } else if (strncmp(buffer, "RESERVER", 8) == 0) {
            int ref, nb;
            char agence[50];
            if (sscanf(buffer + 9, "%d %d %s", &ref, &nb, agence) == 3) {
                reserverVol(newsockfd, NULL, 0, ref, nb, agence, PROTO_TCP, 0);
            } else {
                char err[] = "Requête RESERVER incorrecte\n";
                write(newsockfd, err, strlen(err));
                debug_print("Requête RESERVER incorrecte", NULL, newsockfd);
            }
        } else if (strncmp(buffer, "ANNULER", 7) == 0) {
            int ref, nb;
            char agence[50];
            if (sscanf(buffer + 8, "%d %d %s", &ref, &nb, agence) == 3) {
                annulerVol(newsockfd, NULL, 0, ref, nb, agence, PROTO_TCP, 0);
            } else {
                char err[] = "Requête ANNULER incorrecte\n";
                write(newsockfd, err, strlen(err));
                debug_print("Requête ANNULER incorrecte", NULL, newsockfd);
            }
        } else if (strncmp(buffer, "FACTURE", 7) == 0) {
            char ag[50];
            if (sscanf(buffer + 8, "%s", ag) == 1) {
                consulterFacture(newsockfd, NULL, 0, ag, PROTO_TCP, 0);
            } else {
                char err[] = "Requête FACTURE incorrecte\n";
                write(newsockfd, err, strlen(err));
                debug_print("Requête FACTURE incorrecte", NULL, newsockfd);
            }
        } else {
            char err[] = "Requête non reconnue\n";
            write(newsockfd, err, strlen(err));
            debug_print("Requête non reconnue", NULL, newsockfd);
        }
    }

    close(newsockfd);
    debug_print("Connexion TCP terminée", NULL, newsockfd);
    return NULL;
}

void handle_udp_request(int sockfd, char *buffer, ssize_t n, struct sockaddr_in *cli_addr, socklen_t cli_len) {
    if (n < sizeof(UdpHeader)) {
        char err[] = "Paquet UDP invalide\n";
        UdpHeader header = { 0, "ERR", (uint32_t)strlen(err) };
        char packet[MAX_DATAGRAM_SIZE];
        memcpy(packet, &header, sizeof(UdpHeader));
        memcpy(packet + sizeof(UdpHeader), err, strlen(err));
        sendto(sockfd, packet, sizeof(UdpHeader) + strlen(err), 0, (struct sockaddr *)cli_addr, cli_len);
        debug_print("Paquet UDP invalide : trop court", cli_addr, sockfd);
        return;
    }

    UdpHeader header;
    memcpy(&header, buffer, sizeof(UdpHeader));
    char *payload = buffer + sizeof(UdpHeader);
    payload[header.len] = '\0';

    char debug_msg[BUFFER_SIZE];
    snprintf(debug_msg, sizeof(debug_msg), "Requête traitée : %s", payload);
    debug_print(debug_msg, cli_addr, sockfd);

    if (strncmp(payload, "LIST", 4) == 0) {
        sendVols(sockfd, cli_addr, cli_len, PROTO_UDP, header.seq);
    } else if (strncmp(payload, "RESERVER", 8) == 0) {
        int ref, nb;
        char agence[50];
        if (sscanf(payload + 9, "%d %d %s", &ref, &nb, agence) == 3) {
            reserverVol(sockfd, cli_addr, cli_len, ref, nb, agence, PROTO_UDP, header.seq);
        } else {
            char err[] = "Requête RESERVER incorrecte\n";
            UdpHeader header_out = { header.seq, "ERR", (uint32_t)strlen(err) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header_out, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), err, strlen(err));
            sendto(sockfd, packet, sizeof(UdpHeader) + strlen(err), 0, (struct sockaddr *)cli_addr, cli_len);
            debug_print("Requête RESERVER incorrecte", cli_addr, sockfd);
        }
    } else if (strncmp(payload, "ANNULER", 7) == 0) {
        int ref, nb;
        char agence[50];
        if (sscanf(payload + 8, "%d %d %s", &ref, &nb, agence) == 3) {
            annulerVol(sockfd, cli_addr, cli_len, ref, nb, agence, PROTO_UDP, header.seq);
        } else {
            char err[] = "Requête ANNULER incorrecte\n";
            UdpHeader header_out = { header.seq, "ERR", (uint32_t)strlen(err) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header_out, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), err, strlen(err));
            sendto(sockfd, packet, sizeof(UdpHeader) + strlen(err), 0, (struct sockaddr *)cli_addr, cli_len);
            debug_print("Requête ANNULER incorrecte", cli_addr, sockfd);
        }
    } else if (strncmp(payload, "FACTURE", 7) == 0) {
        char ag[50];
        if (sscanf(payload + 8, "%s", ag) == 1) {
            consulterFacture(sockfd, cli_addr, cli_len, ag, PROTO_UDP, header.seq);
        } else {
            char err[] = "Requête FACTURE incorrecte\n";
            UdpHeader header_out = { header.seq, "ERR", (uint32_t)strlen(err) };
            char packet[MAX_DATAGRAM_SIZE];
            memcpy(packet, &header_out, sizeof(UdpHeader));
            memcpy(packet + sizeof(UdpHeader), err, strlen(err));
            sendto(sockfd, packet, sizeof(UdpHeader) + strlen(err), 0, (struct sockaddr *)cli_addr, cli_len);
            debug_print("Requête FACTURE incorrecte", cli_addr, sockfd);
        }
    } else {
        char err[] = "Requête non reconnue\n";
        UdpHeader header_out = { header.seq, "ERR", (uint32_t)strlen(err) };
        char packet[MAX_DATAGRAM_SIZE];
        memcpy(packet, &header_out, sizeof(UdpHeader));
        memcpy(packet + sizeof(UdpHeader), err, strlen(err));
        sendto(sockfd, packet, sizeof(UdpHeader) + strlen(err), 0, (struct sockaddr *)cli_addr, cli_len);
        debug_print("Requête non reconnue", cli_addr, sockfd);
    }
}

int main(int argc, char *argv[]) {
    if (argc != 2 || (strcmp(argv[1], "tcp") != 0 && strcmp(argv[1], "udp") != 0)) {
        fprintf(stderr, "Usage : %s <tcp|udp>\n", argv[0]);
        return 1;
    }
    Protocol proto = strcmp(argv[1], "tcp") == 0 ? PROTO_TCP : PROTO_UDP;

    int sockfd = -1;
    struct sockaddr_in serv_addr, cli_addr;
    socklen_t clilen = sizeof(cli_addr);

    // Créer le socket
    sockfd = create_socket(proto);
    if (sockfd < 0) {
        return 1;
    }

    // Configurer l'adresse du serveur
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(PORT);

    // Lier le socket
    if (bind(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Échec de l'association du socket");
        close(sockfd);
        return 1;
    }
    debug_print("Socket associé", NULL, sockfd);

    if (proto == PROTO_TCP) {
        // Écouter les connexions
        if (listen(sockfd, 5) < 0) {
            perror("Problème d'écoute sur le socket");
            close(sockfd);
            return 1;
        }
        printf("Lancement du serveur TCP sur le port %d...\n", PORT);
        debug_print("Serveur TCP opérationnel", NULL, sockfd);

        while (1) {
            int *newsockfd = malloc(sizeof(int));
            if (!newsockfd) {
                perror("Échec d'allocation mémoire pour le client");
                continue;
            }
            *newsockfd = accept(sockfd, (struct sockaddr *)&cli_addr, &clilen);
            if (*newsockfd < 0) {
                perror("Problème d'acceptation du client");
                free(newsockfd);
                continue;
            }
            char debug_msg[BUFFER_SIZE];
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &cli_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
            snprintf(debug_msg, sizeof(debug_msg), "Connexion client : %s:%d", client_ip, ntohs(cli_addr.sin_port));
            debug_print(debug_msg, NULL, *newsockfd);

            // Créer un nouveau thread pour le client
            pthread_t thread;
            if (pthread_create(&thread, NULL, handle_tcp_client, newsockfd) != 0) {
                perror("Échec de création du thread client");
                close(*newsockfd);
                free(newsockfd);
                continue;
            }

            // Détacher le thread
            if (pthread_detach(thread) != 0) {
                perror("Problème de détachement du thread");
            }
        }
    } else {
        printf("Lancement du serveur UDP sur le port %d...\n", PORT);
        debug_print("Serveur UDP opérationnel", NULL, sockfd);
        char buffer[MAX_DATAGRAM_SIZE];

        while (1) {
            memset(buffer, 0, MAX_DATAGRAM_SIZE);
            ssize_t n = recvfrom(sockfd, buffer, MAX_DATAGRAM_SIZE, 0, (struct sockaddr *)&cli_addr, &clilen);
            if (n < 0) {
                perror("Problème de réception UDP");
                continue;
            }
            handle_udp_request(sockfd, buffer, n, &cli_addr, clilen);
        }
    }

    close(sockfd);
    debug_print("Socket serveur fermé", NULL, sockfd);
    pthread_mutex_destroy(&vols_mutex);
    pthread_mutex_destroy(&histo_mutex);
    pthread_mutex_destroy(&facture_mutex);
    return 0;
}
