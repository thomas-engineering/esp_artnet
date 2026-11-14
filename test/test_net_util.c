#include "test.h"
#include "net_util.h"

static int valid(const char *ip, const char *m, const char *gw, const char *dns)
{
    const char *why = 0;
    int ok = net_static_config_valid(ip, m, gw, dns, &why);
    CHECK((ok && !why) || (!ok && why));   /* a reason is given exactly on failure */
    return ok;
}

void test_net_util(void)
{
    uint8_t a[4];

    /* parser */
    CHECK(net_parse_ipv4("192.168.1.50", a) && a[0] == 192 && a[3] == 50);
    CHECK(net_parse_ipv4("0.0.0.0", a));
    CHECK(!net_parse_ipv4(NULL, a));
    CHECK(!net_parse_ipv4("", a));
    CHECK(!net_parse_ipv4("1.2", a));
    CHECK(!net_parse_ipv4("1.2.3.4.5", a));
    CHECK(!net_parse_ipv4("256.1.1.1", a));
    CHECK(!net_parse_ipv4("0x7f.0.0.1", a));
    CHECK(!net_parse_ipv4("1.2.3.4 ", a));
    CHECK(!net_parse_ipv4("1.2.3.\"4", a));
    CHECK(!net_parse_ipv4("1.2.3.4\\", a));

    /* masks */
    uint8_t m24[4] = {255, 255, 255, 0}, m0[4] = {0, 0, 0, 0};
    uint8_t m32[4] = {255, 255, 255, 255}, bad[4] = {255, 0, 255, 0};
    uint8_t m30[4] = {255, 255, 255, 252}, m1[4] = {128, 0, 0, 0};
    CHECK(net_mask_valid(m24));
    CHECK(net_mask_valid(m30));
    CHECK(net_mask_valid(m1));
    CHECK(!net_mask_valid(m0));
    CHECK(!net_mask_valid(m32));
    CHECK(!net_mask_valid(bad));

    /* host addresses */
    uint8_t h1[4] = {192, 168, 1, 1}, h2[4] = {0, 1, 2, 3}, h3[4] = {127, 0, 0, 1};
    uint8_t h4[4] = {224, 0, 0, 1}, h5[4] = {255, 255, 255, 255};
    CHECK(net_host_ip_valid(h1));
    CHECK(!net_host_ip_valid(h2));
    CHECK(!net_host_ip_valid(h3));
    CHECK(!net_host_ip_valid(h4));
    CHECK(!net_host_ip_valid(h5));

    /* complete configurations */
    CHECK(valid("192.168.1.50", "255.255.255.0", "192.168.1.1", "8.8.8.8"));
    CHECK(valid("10.0.0.2", "255.255.255.252", "10.0.0.1", "10.0.0.1"));
    CHECK(!valid("192.168.1.50", "255.255.255.0", "192.168.2.1", "8.8.8.8"));  /* gw outside */
    CHECK(!valid("192.168.1.255", "255.255.255.0", "192.168.1.1", "8.8.8.8")); /* broadcast  */
    CHECK(!valid("192.168.1.0", "255.255.255.0", "192.168.1.1", "8.8.8.8"));   /* network    */
    CHECK(!valid("192.168.1.50", "255.0.255.0", "192.168.1.1", "8.8.8.8"));    /* bad mask   */
    CHECK(!valid("192.168.1.50", "255.255.255.0", "192.168.1.50", "8.8.8.8")); /* gw == ip   */
    CHECK(!valid("192.168.1.50", "255.255.255.0", "192.168.1.255", "8.8.8.8"));/* gw bcast   */
    CHECK(!valid("192.168.1.50", "255.255.255.0", "192.168.1.1", "0.0.0.0"));  /* dns        */
    CHECK(!valid("224.1.1.1", "255.255.255.0", "224.1.1.2", "8.8.8.8"));       /* multicast  */
    CHECK(!valid("192.168.1.5\"", "255.255.255.0", "192.168.1.1", "8.8.8.8")); /* quote      */
    CHECK(!valid("", "255.255.255.0", "192.168.1.1", "8.8.8.8"));
}
