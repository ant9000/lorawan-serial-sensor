#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "board.h"

#include "od.h"
#include "msg.h"
#include "thread.h"
#include "ringbuffer.h"

#include "periph/gpio.h"
#include "periph/uart.h"

#include "net/gnrc/netif.h"
#include "net/gnrc/netif/hdr.h"
#include "net/gnrc/netreg.h"
#include "net/gnrc/pktdump.h"

#define LORAWAN_IFACE       3
#define LORAWAN_DST_PORT    42

#define UART_PORT           UART_DEV(1)
#define UART_SPEED          115200UL
#define UART_BUFSIZE        (128U)

#define MSG_CMD_COMPLETED   1
#define MSG_TIMEOUT         2

typedef struct {
    char rx_mem[UART_BUFSIZE];
    ringbuffer_t rx_buf;
} uart_ctx_t;

static uart_ctx_t ctx;
static kernel_pid_t main_thread;

static void rx_cb(void *arg, uint8_t data)
{
    (void)arg;

    ringbuffer_add_one(&ctx.rx_buf, data);

    if (data == '\n') {
        msg_t msg;
        msg.content.value = MSG_CMD_COMPLETED;
        msg_send(&msg, main_thread);
    }
}

int lorawan_send(netif_t *iface)
{
    uint8_t addr[] = {LORAWAN_DST_PORT};
    size_t addr_len = 1;
    gnrc_pktsnip_t *pkt, *hdr;
    gnrc_netif_hdr_t *nethdr;
    char buffer[UART_BUFSIZE];
    size_t n = 0;
    int c;

    do {
        c = ringbuffer_get_one(&ctx.rx_buf);
        if ((c == '\n') || (c == -1)) break;
        buffer[n++] = c;
    } while (n < UART_BUFSIZE);

    od_hex_dump(buffer, n, 0);

    pkt = gnrc_pktbuf_add(NULL, buffer, n, GNRC_NETTYPE_UNDEF);
    if (pkt == NULL) {
        printf("error: cannot allocate packet buffer\n");
        return 1;
    }
    hdr = gnrc_netif_hdr_build(NULL, 0, addr, addr_len);
    if (hdr == NULL) {
        printf("error: packet buffer full\n");
        gnrc_pktbuf_release(pkt);
        return 1;
    }
    pkt = gnrc_pkt_prepend(pkt, hdr);
    nethdr = (gnrc_netif_hdr_t *)hdr->data;
    nethdr->flags = 0x00;
    /* and send it */
    if (gnrc_netif_send(container_of(iface, gnrc_netif_t, netif), pkt) < 1) {
        printf("error: unable to send\n");
        gnrc_pktbuf_release(pkt);
        return 1;
    }

    return 0;
}

int main(void)
{
    msg_t msg;
    msg_t msg_queue[8];
    msg_init_queue(msg_queue, 8);
    main_thread = thread_getpid();
    netif_t *lorawan;

    puts("LoRaWAN serial sensor");

    /* Initalize LoRaWAN interface */
    gpio_set(TCXO_PWR_PIN);
    gnrc_netreg_entry_t dump = GNRC_NETREG_ENTRY_INIT_PID(GNRC_NETREG_DEMUX_CTX_ALL,
                                                          gnrc_pktdump_pid);
    gnrc_netreg_register(GNRC_NETTYPE_UNDEF, &dump);
    lorawan = netif_get_by_id(LORAWAN_IFACE);
    netopt_enable_t en = NETOPT_ENABLE;
    if (netif_set_opt(lorawan, NETOPT_LINK, 0, &en, sizeof(en)) < 0) {
        puts("ERROR: unable to set link up");
    }
    printf("Success: Initialized LoRaWAN interface\n");

    /* initialize UART */
    ringbuffer_init(&ctx.rx_buf, ctx.rx_mem, UART_BUFSIZE);
    int res = uart_init(UART_PORT, UART_SPEED, rx_cb, NULL);
    if (res == UART_NOBAUD) {
        printf("Error: Given baudrate (%lu) not possible\n", UART_SPEED);
        return 1;
    }
    else if (res != UART_OK) {
        puts("Error: Unable to initialize UART device");
        return 1;
    }
    printf("Success: Initialized UART at BAUD %lu\n", UART_SPEED);

    while (1) {
        msg_receive(&msg);
        switch(msg.content.value) {
            case MSG_CMD_COMPLETED:
                lorawan_send(lorawan);
                break;
            default:
                break;
        }
    }

    /* this should never be reached */
    return 0;
}
