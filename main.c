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

#define UPDATER_PRIO        (THREAD_PRIORITY_MAIN - 1)

#define LORAWAN_IFACE       3
#define LORAWAN_DST_PORT    42
#define LORAWAN_MAX_SIZE    64

#define UART_PORT           UART_DEV(1)
#define UART_SPEED          115200UL
#define UART_BUF_SIZE       (128U)

typedef struct {
    float temperature;
    ztimer_now_t temperature_tstamp;
    uint32_t failure_count;
    ztimer_now_t failure_count_tstamp;
} sensor_state_t;

typedef struct {
    char rx_mem[UART_BUF_SIZE];
    ringbuffer_t rx_buf;
} uart_ctx_t;

static sensor_state_t sensor_state;
static uart_ctx_t ctx;
static kernel_pid_t updater_pid;
static char updater_stack[THREAD_STACKSIZE_MAIN];
static netif_t *lorawan;


static void rx_cb(void *arg, uint8_t data)
{
    (void)arg;

    ringbuffer_add_one(&ctx.rx_buf, data);

    if (data == '\r') {
        msg_t msg;
        msg.content.value = 1; // ignored
        msg_send(&msg, updater_pid);
    }
}

int lorawan_send(netif_t *iface, char *buffer, size_t n)
{
    uint8_t addr[] = {LORAWAN_DST_PORT};
    size_t addr_len = 1;
    gnrc_pktsnip_t *pkt, *hdr;
    gnrc_netif_hdr_t *nethdr;

    printf("### Sending packet (%d bytes): ###\n", n);
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
    puts("### Sent. ###");

    return 0;
}

void update_sensor_state(void);

static void *updater(void *arg)
{
    (void)arg;
    msg_t msg;
    msg_t msg_queue[8];
    msg_init_queue(msg_queue, 8);
    while (1) {
        msg_receive(&msg);
        update_sensor_state();
    }
    // never reached
    return NULL;
}

void update_sensor_state(void)
{
    char buffer[UART_BUF_SIZE+1], *ptr;
    size_t n = 0;
    int c;
    do {
        c = ringbuffer_get_one(&ctx.rx_buf);
        if ((c == '\n') || (c == -1) || (c == '\r')) break;
        buffer[n++] = c;
    } while (n < UART_BUF_SIZE);
    buffer[n] = 0;

    puts("### Received line: ###");
    od_hex_dump(buffer, n, 0);

    ptr = buffer+1;
    switch(buffer[0]) {
        case 'T':
            // temperature
            sensor_state.temperature = strtof(ptr+2, NULL);
            sensor_state.temperature_tstamp = ztimer_now(ZTIMER_SEC);
            break;
        case 'F':
            // failure
            sensor_state.failure_count = strtoul(ptr, NULL, 10);
            sensor_state.failure_count_tstamp = ztimer_now(ZTIMER_SEC);
            break;
        default:
            // ignore
            break;
    }
}

void send_sensor_state(void)
{
    char buffer[LORAWAN_MAX_SIZE];

    snprintf(
        buffer, LORAWAN_MAX_SIZE,
        "%.1f,%lu,%lu,%lu",
        sensor_state.temperature,
        ztimer_now(ZTIMER_SEC) - sensor_state.temperature_tstamp,
        sensor_state.failure_count,
        ztimer_now(ZTIMER_SEC) - sensor_state.failure_count_tstamp
    );

    lorawan_send(lorawan, buffer, strlen(buffer));
}

void send_message(char *data)
{
    char buffer[LORAWAN_MAX_SIZE];

    snprintf(
        buffer, LORAWAN_MAX_SIZE,
        "%s",data
    );

    lorawan_send(lorawan, buffer, strlen(buffer));
}

int main(void)
{
    sensor_state.temperature = -999;
    sensor_state.temperature_tstamp = 0;
    sensor_state.failure_count = 0;
    sensor_state.failure_count_tstamp = 0;

    puts("LoRaWAN serial sensor");

    /* start the sensor updater thread */
    updater_pid = thread_create(updater_stack, sizeof(updater_stack),
                                UPDATER_PRIO, 0, updater, NULL, "updater");

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
    ringbuffer_init(&ctx.rx_buf, ctx.rx_mem, UART_BUF_SIZE);
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
	ztimer_sleep(ZTIMER_SEC, 10);
    send_message("Start Node");
	ztimer_sleep(ZTIMER_SEC, 30);

    while (1) {
        if (sensor_state.temperature != -999 || sensor_state.failure_count != 0) {
            puts("Sensor data available, sending");
            send_sensor_state();
        } else {
            puts("Nothing to send: send No Data");
			send_message("No Data");
            
        }
        ztimer_sleep(ZTIMER_SEC, 30);
    }

    /* this should never be reached */
    return 0;
}
