<div align="center">
  <h1>Example 04: TCP HTTP Client</h1>
  <p><em>Utilizing lwIP to establish an outbound TCP connection.</em></p>
</div>

---

This advanced example demonstrates the steps required to use the raw network system calls to establish a connection with an external HTTP server and dump the response over the terminal.

## 📝 Concepts Introduced
* Verifying the network state (`sys_network_is_initialized`, `sys_network_has_ip`).
* Performing DNS lookups manually via `sys_dns_lookup`.
* Managing strict TCP flow logic (`sys_tcp_connect`, send, block for receive).
* Using the terminal `SYS_WRITE` output for debugging.
* Declaring app metadata via source annotations.

---

## The Code (e.g. `contrib/netutils/src/http_get.c`)

```c
// BOREDOS_APP_DESC: HTTP GET client — fetches a webpage over TCP.
#include <stdlib.h>
#include <string.h>
#include <syscall.h>

int main(void) {
  if (!sys_network_is_initialized() || !sys_network_has_ip()) {
    printf("Network is unreachable! Make sure you inited the network first!\n");
    return 1;
  }

  // 1. Resolve host name to IP
  const char *target_host = "boreddev.nl";
  net_ipv4_address_t server_ip;

  printf("Resolving %s...\n", target_host);
  if (sys_dns_lookup(target_host, &server_ip) < 0) {
    printf("DNS Lookup failed.\n");
    return 1;
  }
  printf("Resolved to: %d.%d.%d.%d\n", server_ip.bytes[0], server_ip.bytes[1],
         server_ip.bytes[2], server_ip.bytes[3]);

  // 2. Establish a TCP connection on port 80 (HTTP)
  printf("Connecting...\n");
  if (sys_tcp_connect(&server_ip, 80) < 0) {
    printf("Connection failed.\n");
    return 1;
  }
  printf("Connected! Sending GET request...\n");

  // 3. Format and send the raw HTTP Request
  char request[256];
  strcpy(request, "GET / HTTP/1.1\r\nHost: ");
  strcat(request, target_host);
  strcat(request, "\r\nConnection: close\r\n\r\n");

  if (sys_tcp_send(request, strlen(request)) < 0) {
    printf("Failed to send data.\n");
    sys_tcp_close();
    return 1;
  }

  // 4. Block and wait for response data
  char recv_buf[512];
  int bytes_received;

  printf("\n--- RESPONSE ---\n");
  while ((bytes_received = sys_tcp_recv(recv_buf, sizeof(recv_buf) - 1)) > 0) {
    recv_buf[bytes_received] = '\0'; // Null-terminate the chunk
    printf("%s", recv_buf);          // Print the chunk to stdout
  }

  // 5. Cleanup
  printf("\n--- END RESPONSE ---\n");
  sys_tcp_close();
  printf("Connection closed.\n");

  return 0;
}
```

## How it Works

1.  **Network Setup**: First, we must ensure the host machine or QEMU environment gave BoredOS a valid IP address via DHCP. The `sys_network_has_ip()` check prevents our app from hanging trying to route data to nowhere.
2.  **DNS (`sys_dns_lookup`)**: Since we want to connect to a domain name, not a raw IP, we query the DNS server configured by the OS (which it received via DHCP).
3.  **Connection (`sys_tcp_connect`)**: We block the application thread while the OS performs the 3-way TCP handshake over port 80.
4.  **Payload (`sys_tcp_send`)**: We format a compliant HTTP/1.1 payload representing a simple GET request for the root directory `/`.
5.  **Chunked Receiving (`sys_tcp_recv`)**: The server's response might be larger than our `recv_buf` (512 bytes). Therefore, we loop. `sys_tcp_recv` blocks execution until data arrives. If it returns `0`, the remote server cleanly closed the connection (which happens automatically because we specified `Connection: close` in our request payload!).

## Running It

Make sure QEMU is running with networking enabled. Launch the terminal and type `http_get`. You will see the raw headers and HTML source of the target webpage scroll down the CLI interface!
