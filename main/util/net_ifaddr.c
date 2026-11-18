#include "net_ifaddr.h"

#include "esp_netif.h"
#include "net_guard.h"

bool net_own_ipv4(uint32_t *ip_out, uint32_t *mask_out)
{
    if (!ip_out || !mask_out) return false;
    // STA only. This firmware never brings up a SoftAP (no
    // esp_netif_create_default_wifi_ap, no WIFI_MODE_AP anywhere in main/), so
    // WIFI_STA_DEF is the whole story. If an AP mode is ever added, every
    // caller of this - the OTA plaintext rule, the Cloudlog plaintext rule and
    // the TX interlock - silently decides on the wrong interface, so add the
    // AP netif HERE rather than at any of them.
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return false;
    *ip_out   = net_ipv4_from_network_order(info.ip.addr);
    *mask_out = net_ipv4_from_network_order(info.netmask.addr);
    return true;
}
