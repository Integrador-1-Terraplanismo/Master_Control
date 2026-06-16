#ifndef WIFI_TCP_MGR_H
#define WIFI_TCP_MGR_H

// Respostas possíveis para o remetente
typedef enum {
    TCP_REPLY_ACK,   
    TCP_REPLY_RECD,  
    TCP_REPLY_NRECD, 
    TCP_REPLY_NPLT,  
    TCP_REPLY_TIMEOUT, // "tempo esgotado (timeout)"
    TCP_REPLY_BUSY     // "sistema ocupado (busy)"
} tcp_reply_t;

void wifi_tcp_init(void);
void tcp_send_reply(tcp_reply_t reply_type);
void wifi_tcp_send_raw(const char* data);

#endif