/*
    This file is part of Kismet

    Kismet is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Kismet is distributed in the hope that it will be useful,
      but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Kismet; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef __KCOMPAT_PLATFORM_H__
#define __KCOMPAT_PLATFORM_H__

/* Host platform interfaces a USB adapter has no part of: ACPI and DMI (laptop SAR tables
 * and quirks), multicolor LEDs and sysfs groups, plus the IPv4 headers drivers peek at
 * on transmit.  ACPI and DMI find nothing, so drivers take their no-platform paths. */

#include "stubs.h"

/* uuid.h */
typedef struct {
    u8 b[16];
} guid_t;
/* uuid_t is left out: macOS libc has its own */
#define GUID_INIT(a, b, c, d0, d1, d2, d3, d4, d5, d6, d7) ((guid_t) {{ \
    (a) & 0xff, ((a) >> 8) & 0xff, ((a) >> 16) & 0xff, ((a) >> 24) & 0xff, \
    (b) & 0xff, ((b) >> 8) & 0xff, (c) & 0xff, ((c) >> 8) & 0xff, \
    (d0), (d1), (d2), (d3), (d4), (d5), (d6), (d7) }})
#define UUID_SIZE 16
static inline bool guid_equal(const guid_t *a, const guid_t *b) { return memcmp(a, b, sizeof(*a)) == 0; }

/* acpi.h: no ACPI namespace, so every lookup fails */
typedef void *acpi_handle;
typedef u32 acpi_status;
typedef char *acpi_string;
typedef u64 acpi_size;
typedef u32 acpi_object_type;
#define AE_OK ((acpi_status) 0)
#define AE_NOT_FOUND ((acpi_status) 5)
#define ACPI_SUCCESS(s) ((s) == AE_OK)
#define ACPI_FAILURE(s) ((s) != AE_OK)
#define ACPI_TYPE_ANY 0
#define ACPI_TYPE_INTEGER 1
#define ACPI_TYPE_STRING 2
#define ACPI_TYPE_BUFFER 3
#define ACPI_TYPE_PACKAGE 4
union acpi_object {
    acpi_object_type type;
    struct {
        acpi_object_type type;
        u64 value;
    } integer;
    struct {
        acpi_object_type type;
        u32 length;
        char *pointer;
    } string;
    struct {
        acpi_object_type type;
        u32 length;
        u8 *pointer;
    } buffer;
    struct {
        acpi_object_type type;
        u32 count;
        union acpi_object *elements;
    } package;
};
struct acpi_object_list {
    u32 count;
    union acpi_object *pointer;
};
struct acpi_buffer {
    acpi_size length;
    void *pointer;
};
#define ACPI_ALLOCATE_BUFFER ((acpi_size) -1)
#define ACPI_FREE(p) kfree(p)
#define ACPI_HANDLE(dev) ((void) (dev), (acpi_handle) NULL)
#define ACPI_COMPANION(dev) ((void) (dev), NULL)
#define has_acpi_companion(dev) ((void) (dev), false)
static inline acpi_status acpi_get_handle(acpi_handle parent, acpi_string path, acpi_handle *h) {
    (void) parent; (void) path; *h = NULL; return AE_NOT_FOUND;
}
static inline acpi_status acpi_evaluate_object(acpi_handle h, acpi_string path,
        struct acpi_object_list *args, struct acpi_buffer *ret) {
    (void) h; (void) path; (void) args; (void) ret; return AE_NOT_FOUND;
}
static inline union acpi_object *acpi_evaluate_dsm(acpi_handle h, const guid_t *guid, u64 rev,
        u64 func, union acpi_object *argv4) {
    (void) h; (void) guid; (void) rev; (void) func; (void) argv4; return NULL;
}
static inline union acpi_object *acpi_evaluate_dsm_typed(acpi_handle h, const guid_t *guid,
        u64 rev, u64 func, union acpi_object *argv4, acpi_object_type type) {
    (void) type; return acpi_evaluate_dsm(h, guid, rev, func, argv4);
}
static inline bool acpi_check_dsm(acpi_handle h, const guid_t *guid, u64 rev, u64 funcs) {
    (void) h; (void) guid; (void) rev; (void) funcs; return false;
}

/* dmi.h: no SMBIOS tables, so no quirk matches */
enum dmi_field {
    DMI_NONE,
    DMI_BIOS_VENDOR,
    DMI_BIOS_VERSION,
    DMI_BIOS_DATE,
    DMI_SYS_VENDOR,
    DMI_PRODUCT_NAME,
    DMI_PRODUCT_VERSION,
    DMI_PRODUCT_SERIAL,
    DMI_PRODUCT_UUID,
    DMI_PRODUCT_SKU,
    DMI_PRODUCT_FAMILY,
    DMI_BOARD_VENDOR,
    DMI_BOARD_NAME,
    DMI_BOARD_VERSION,
};
struct dmi_strmatch {
    unsigned char slot:7;
    unsigned char exact_match:1;
    char substr[79];
};
struct dmi_system_id {
    int (*callback)(const struct dmi_system_id *);
    const char *ident;
    struct dmi_strmatch matches[4];
    void *driver_data;
};
#define DMI_MATCH(a, b) { .slot = a, .substr = b }
#define DMI_EXACT_MATCH(a, b) { .slot = a, .substr = b, .exact_match = 1 }
static inline const struct dmi_system_id *dmi_first_match(const struct dmi_system_id *list) {
    (void) list; return NULL;
}
static inline int dmi_check_system(const struct dmi_system_id *list) { (void) list; return 0; }
static inline const char *dmi_get_system_info(int field) { (void) field; return NULL; }

/* led-class-multicolor.h */
struct mc_subled {
    unsigned int color_index;
    unsigned int brightness;
    unsigned int intensity;
    unsigned int channel;
};
struct led_classdev_mc {
    struct led_classdev led_cdev;
    unsigned int num_colors;
    struct mc_subled *subled_info;
};
static inline int led_classdev_multicolor_register(struct device *p, struct led_classdev_mc *l) {
    (void) p; (void) l; return 0;
}
static inline void led_classdev_multicolor_unregister(struct led_classdev_mc *l) { (void) l; }
#define devm_led_classdev_multicolor_register(d, l) led_classdev_multicolor_register(d, l)
static inline int led_mc_calc_color_components(struct led_classdev_mc *l, enum led_brightness b) {
    (void) l; (void) b; return 0;
}

/* sysfs: attribute groups are kept on the device but never shown */
struct kobject;
#define kobj_to_dev(k) ((void) (k), (struct device *) NULL)
#define DEVICE_ATTR(n, m, s, st) \
    struct device_attribute dev_attr_##n = { .attr = { #n, m }, .show = s, .store = st }
#define DEVICE_ATTR_RO(n) DEVICE_ATTR(n, 0444, n##_show, NULL)
#define DEVICE_ATTR_RW(n) DEVICE_ATTR(n, 0644, n##_show, n##_store)
#define DEVICE_ATTR_WO(n) DEVICE_ATTR(n, 0200, NULL, n##_store)
#define DEFINE_SIMPLE_SYSFS_GROUP_VISIBLE(name) \
    static inline umode_t sysfs_group_visible_##name(struct kobject *kobj, struct attribute *attr, int n) { \
        (void) attr; (void) n; return name##_group_visible(kobj) ? 0444 : 0; \
    }
#define SYSFS_GROUP_VISIBLE(name) sysfs_group_visible_##name
#define __ATTRIBUTE_GROUPS(name) \
    static const struct attribute_group *name##_groups[] = { &name##_group, NULL }

/* Network byte order, as the kernel's byteorder/generic.h.  macOS libc defines these from
 * <sys/types.h>; glibc only in <arpa/inet.h> and <netinet/in.h>, which kcompat doesn't
 * include */
#ifndef htons
#define htons(x) cpu_to_be16(x)
#endif
#ifndef ntohs
#define ntohs(x) be16_to_cpu(x)
#endif
#ifndef htonl
#define htonl(x) cpu_to_be32(x)
#endif
#ifndef ntohl
#define ntohl(x) be32_to_cpu(x)
#endif

/* IPv4, UDP and ARP headers, as the kernel lays them out */
#ifndef IPPROTO_ICMP
#define IPPROTO_ICMP 1
#endif
#ifndef IPPROTO_TCP
#define IPPROTO_TCP 6
#endif
#ifndef IPPROTO_UDP
#define IPPROTO_UDP 17
#endif
struct iphdr {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    u8 version:4, ihl:4;
#else
    u8 ihl:4, version:4;
#endif
    u8 tos;
    __be16 tot_len;
    __be16 id;
    __be16 frag_off;
    u8 ttl;
    u8 protocol;
    __sum16 check;
    __be32 saddr;
    __be32 daddr;
};
struct udphdr {
    __be16 source;
    __be16 dest;
    __be16 len;
    __sum16 check;
};
#define ARPHRD_ETHER 1
#define ARPOP_REQUEST 1
#define ARPOP_REPLY 2
struct arphdr {
    __be16 ar_hrd;
    __be16 ar_pro;
    u8 ar_hln;
    u8 ar_pln;
    __be16 ar_op;
};
static inline struct iphdr *ip_hdr(const struct sk_buff *skb) {
    return (struct iphdr *) skb_network_header(skb);
}
static inline struct udphdr *udp_hdr(const struct sk_buff *skb) {
    return (struct udphdr *) skb_transport_header(skb);
}
static inline struct arphdr *arp_hdr(const struct sk_buff *skb) {
    return (struct arphdr *) skb_network_header(skb);
}

#endif
