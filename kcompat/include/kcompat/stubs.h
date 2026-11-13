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

#ifndef __KCOMPAT_STUBS_H__
#define __KCOMPAT_STUBS_H__

/* Kernel subsystems with no userspace equivalent.  Types exist so headers and
 * unused code paths compile; functions report "not present". */

#include "net.h"

/* Device tree */
struct device_node {
    const char *name;
};
struct property {
    const char *name;
    int length;
    void *value;
};
static inline struct device_node *of_get_child_by_name(const struct device_node *n, const char *name) {
    (void) n; (void) name; return NULL;
}
static inline struct device_node *of_find_node_by_phandle(u32 h) { (void) h; return NULL; }
static inline struct device_node *of_parse_phandle(const struct device_node *n, const char *p, int i) {
    (void) n; (void) p; (void) i; return NULL;
}
static inline struct property *of_find_property(const struct device_node *n, const char *name, int *len) {
    (void) n; (void) name; if (len) *len = 0; return NULL;
}
static inline const void *of_get_property(const struct device_node *n, const char *name, int *len) {
    (void) n; (void) name; if (len) *len = 0; return NULL;
}
static inline int of_property_read_u32(const struct device_node *n, const char *name, u32 *v) {
    (void) n; (void) name; (void) v; return -ENOENT;
}
static inline int of_property_read_u8(const struct device_node *n, const char *name, u8 *v) {
    (void) n; (void) name; (void) v; return -ENOENT;
}
static inline int of_property_read_string(const struct device_node *n, const char *name, const char **s) {
    (void) n; (void) name; (void) s; return -ENOENT;
}
static inline bool of_property_read_bool(const struct device_node *n, const char *name) {
    (void) n; (void) name; return false;
}
static inline bool of_device_is_available(const struct device_node *n) { (void) n; return false; }
static inline bool of_device_is_compatible(const struct device_node *n, const char *c) {
    (void) n; (void) c; return false;
}
static inline const char *of_prop_next_string(struct property *p, const char *cur) {
    (void) p; (void) cur; return NULL;
}
static inline int of_get_mac_address(struct device_node *n, u8 *addr) { (void) n; (void) addr; return -ENODEV; }
static inline void of_node_put(struct device_node *n) { (void) n; }
static inline struct device_node *of_node_get(struct device_node *n) { return n; }
#define for_each_child_of_node(parent, child) for ((child) = NULL; (child); )
#define of_property_for_each_string(np, propname, prop, s) for ((prop) = NULL, (s) = NULL; (s); )
#define of_property_read_u32_array(n, name, v, c) (-ENOENT)

/* nvmem, mtd, regmap */
struct nvmem_cell;
static inline struct nvmem_cell *of_nvmem_cell_get(struct device_node *n, const char *id) {
    (void) n; (void) id; return ERR_PTR(-ENOENT);
}
static inline void *nvmem_cell_read(struct nvmem_cell *c, size_t *len) { (void) c; (void) len; return ERR_PTR(-ENOENT); }
static inline void nvmem_cell_put(struct nvmem_cell *c) { (void) c; }
struct mtd_info;
static inline struct mtd_info *get_mtd_device_nm(const char *n) { (void) n; return ERR_PTR(-ENODEV); }
static inline int mtd_read(struct mtd_info *m, loff_t f, size_t l, size_t *r, u_char *b) {
    (void) m; (void) f; (void) l; (void) r; (void) b; return -ENODEV;
}
static inline bool mtd_is_bitflip(int e) { (void) e; return false; }
static inline void put_mtd_device(struct mtd_info *m) { (void) m; }
struct regmap;
static inline int regmap_read(struct regmap *m, unsigned int r, unsigned int *v) { (void) m; (void) r; *v = 0; return -ENODEV; }
static inline int regmap_write(struct regmap *m, unsigned int r, unsigned int v) { (void) m; (void) r; (void) v; return -ENODEV; }

/* LEDs */
enum led_brightness {
    LED_OFF = 0,
    LED_ON = 1,
    LED_HALF = 127,
    LED_FULL = 255,
};
struct led_classdev {
    const char *name;
    unsigned int brightness;
    unsigned int max_brightness;
    const char *default_trigger;
    void (*brightness_set)(struct led_classdev *, enum led_brightness);
    int (*blink_set)(struct led_classdev *, unsigned long *, unsigned long *);
};
static inline int led_classdev_register(struct device *p, struct led_classdev *l) { (void) p; (void) l; return 0; }
static inline void led_classdev_unregister(struct led_classdev *l) { (void) l; }
#define devm_led_classdev_register(d, l) led_classdev_register(d, l)

/* sysfs attributes, hwmon and thermal */
struct attribute {
    const char *name;
    umode_t mode;
};
struct attribute_group {
    const char *name;
    struct attribute **attrs;
};
struct device_attribute {
    struct attribute attr;
    ssize_t (*show)(struct device *, struct device_attribute *, char *);
    ssize_t (*store)(struct device *, struct device_attribute *, const char *, size_t);
};
struct sensor_device_attribute {
    struct device_attribute dev_attr;
    int index;
};
#define to_sensor_dev_attr(a) container_of(a, struct sensor_device_attribute, dev_attr)
#define SENSOR_DEVICE_ATTR(n, m, s, st, i) \
    struct sensor_device_attribute sensor_dev_attr_##n = { .dev_attr = { .attr = { #n, m }, .show = s, .store = st }, .index = i }
#define SENSOR_DEVICE_ATTR_RO(n, f, i) SENSOR_DEVICE_ATTR(n, 0444, f##_show, NULL, i)
#define SENSOR_DEVICE_ATTR_RW(n, f, i) SENSOR_DEVICE_ATTR(n, 0644, f##_show, f##_store, i)
#define SENSOR_DEVICE_ATTR_WO(n, f, i) SENSOR_DEVICE_ATTR(n, 0200, NULL, f##_store, i)
#define ATTRIBUTE_GROUPS(name) \
    static const struct attribute_group name##_group = { .attrs = name##_attrs }; \
    static const struct attribute_group *name##_groups[] = { &name##_group, NULL }
#define sysfs_emit(buf, fmt, ...) sprintf(buf, fmt, ##__VA_ARGS__)
static inline struct device *devm_hwmon_device_register_with_groups(struct device *d, const char *n,
        void *data, const struct attribute_group **g) {
    (void) d; (void) n; (void) data; (void) g; return NULL;
}
struct thermal_cooling_device;
struct thermal_cooling_device_ops {
    int (*get_max_state)(struct thermal_cooling_device *, unsigned long *);
    int (*get_cur_state)(struct thermal_cooling_device *, unsigned long *);
    int (*set_cur_state)(struct thermal_cooling_device *, unsigned long);
};

/* debugfs */
struct dentry;
struct seq_file {
    void *private;
};
struct poll_table_struct;
typedef struct poll_table_struct poll_table;
typedef unsigned int __poll_t;
/* An open file, for character devices the host calls into directly */
struct inode {
    void *i_private;
};
struct file {
    void *private_data;
    unsigned int f_flags;
};
struct file_operations {
    struct module *owner;
    int (*open)(struct inode *inode, struct file *file);
    int (*release)(struct inode *inode, struct file *file);
    ssize_t (*read)(struct file *file, char *buf, size_t count, loff_t *ppos);
    ssize_t (*write)(struct file *file, const char *buf, size_t count, loff_t *ppos);
    __poll_t (*poll)(struct file *file, poll_table *wait);
    long (*unlocked_ioctl)(struct file *file, unsigned int cmd, unsigned long arg);
    long (*compat_ioctl)(struct file *file, unsigned int cmd, unsigned long arg);
    loff_t (*llseek)(struct file *file, loff_t offset, int whence);
};
/* One address space: user copies are plain copies */
static inline unsigned long copy_from_user(void *to, const void *from, unsigned long n) {
    memcpy(to, from, n);
    return 0;
}
static inline unsigned long copy_to_user(void *to, const void *from, unsigned long n) {
    memcpy(to, from, n);
    return 0;
}
static inline ssize_t simple_read_from_buffer(void *to, size_t count, loff_t *ppos, const void *from,
        size_t available) {
    loff_t pos = *ppos;

    if (pos < 0)
        return -EINVAL;
    if ((size_t) pos >= available || count == 0)
        return 0;
    if (count > available - (size_t) pos)
        count = available - (size_t) pos;
    memcpy(to, (const char *) from + pos, count);
    *ppos = pos + (loff_t) count;
    return (ssize_t) count;
}
static inline int simple_open(struct inode *inode, struct file *file) {
    if (inode->i_private)
        file->private_data = inode->i_private;
    return 0;
}
#define default_llseek NULL
#define no_llseek NULL
static inline char *strim(char *s) {
    size_t n = strlen(s);

    while (n > 0 && isspace((unsigned char) s[n - 1]))
        s[--n] = '\0';
    while (*s && isspace((unsigned char) *s))
        s++;
    return s;
}
#ifndef O_NONBLOCK
#define O_NONBLOCK 04000
#endif
/* debugfs files are never created; their file operations only need to build */
#define single_open(file, show, data) 0
#define seq_read NULL
#define seq_lseek NULL
#define single_release NULL
#define fixed_size_llseek(file, offset, whence, size) ((loff_t) 0)
#define compat_ptr_ioctl NULL
#define nonseekable_open(inode, file) 0
#define poll_wait(file, wq, table) do { (void) (file); (void) (wq); (void) (table); } while (0)
#define EPOLLIN 0x0001
#define EPOLLRDNORM 0x0040
#define EPOLLHUP 0x0010

/* Character devices: registered by name, opened by the host through their file operations */
#define MISC_DYNAMIC_MINOR 255
struct miscdevice {
    int minor;
    const char *name;
    const struct file_operations *fops;
};
int misc_register(struct miscdevice *misc);
void misc_deregister(struct miscdevice *misc);
/* The registered device of that name, or NULL; open it with file->private_data = misc, as
 * the kernel's misc core does */
struct miscdevice *kc_misc_find(const char *name);

#define debugfs_create_dir(n, p) ((struct dentry *) NULL)
#define debugfs_create_file(n, m, p, d, f) ((struct dentry *) NULL)
#define debugfs_create_u8(n, m, p, v) do { } while (0)
#define debugfs_create_u16(n, m, p, v) do { } while (0)
#define debugfs_create_u32(n, m, p, v) do { } while (0)
#define debugfs_create_bool(n, m, p, v) do { } while (0)
#define debugfs_create_devm_seqfile(d, n, p, f) do { } while (0)
#define debugfs_remove_recursive(d) do { } while (0)
#define seq_printf(s, fmt, ...) do { (void) (s); } while (0)
#define seq_puts(s, str) do { (void) (s); } while (0)

struct debugfs_blob_wrapper {
    void *data;
    unsigned long size;
};

/* devcoredump */
static inline void dev_coredumpv(struct device *dev, void *data, size_t len, gfp_t gfp) {
    (void) dev; (void) len; (void) gfp; kfree(data);
}

/* MediaTek WED / Airoha NPU offload, never present; out-of-tree mt76 carries its own
 * airoha_offload.h, which these replace */
#define AIROHA_OFFLOAD_H
struct mtk_wed_device {
    bool init_done;
    bool running;
    int wdma_idx;
    struct device *dev;
    u32 version;
    u32 rev_id;
};
#define mtk_wed_device_active(d) false
#define mtk_wed_get_rx_capa(d) false
#define mtk_wed_device_reg_read(d, r) 0
#define mtk_wed_device_reg_write(d, r, v) do { } while (0)
#define mtk_wed_device_irq_get(d, m) 0
#define mtk_wed_device_irq_set_mask(d, m) do { } while (0)
#define mtk_wed_device_detach(d) do { } while (0)
#define mtk_wed_device_start(d, m) do { } while (0)
#define mtk_wed_device_stop(d) do { } while (0)
#define mtk_wed_device_dma_reset(d) do { } while (0)
#define mtk_wed_device_ppe_check(d, s, r, h) do { } while (0)
#define mtk_wed_device_update_msg(d, i, data, len) (-ENODEV)
#define mtk_wed_device_setup_tc(d, n, t, data) (-EOPNOTSUPP)
#define mtk_wed_is_amsdu_supported(d) false
#define is_mtk_wed_device_active(d) false
struct airoha_npu;
struct airoha_ppe_dev;
enum airoha_npu_wlan_set_cmd {
    WLAN_FUNC_SET_WAIT_PCIE_ADDR,
};
enum airoha_npu_wlan_get_cmd {
    WLAN_FUNC_GET_WAIT_NPU_INFO,
};
#define airoha_npu_wlan_send_msg(npu, ifindex, cmd, val, len, gfp) (-EOPNOTSUPP)
#define airoha_npu_wlan_get_msg(npu, ifindex, cmd, val, len, gfp) (-EOPNOTSUPP)

/* SDIO and MMIO buses, never present */
struct sdio_func;
struct mmc_host;
struct pci_dev;
struct platform_device;
static inline u32 readl(const volatile void *a) { (void) a; WARN_ON(1); return 0xffffffff; }
static inline void writel(u32 v, volatile void *a) { (void) v; (void) a; WARN_ON(1); }
#define readl_relaxed readl
#define writel_relaxed writel
#define memcpy_toio(d, s, n) memcpy((void *) (d), s, n)
#define memcpy_fromio(d, s, n) memcpy(d, (const void *) (s), n)
#define memset_io(d, v, n) memset((void *) (d), v, n)
#define ioremap(a, s) NULL
#define iounmap(a) do { } while (0)

/* Interrupts */
#define IRQ_NONE 0
#define IRQ_HANDLED 1
typedef int irqreturn_t;
void tasklet_setup(struct tasklet_struct *t, void (*cb)(struct tasklet_struct *));
void tasklet_schedule(struct tasklet_struct *t);
void tasklet_kill(struct tasklet_struct *t);
#define tasklet_disable(t) do { (void) (t); } while (0)
#define tasklet_enable(t) do { (void) (t); } while (0)
#define from_tasklet(var, t, field) container_of(t, typeof(*var), field)
#define disable_irq(i) do { } while (0)
#define enable_irq(i) do { } while (0)
#define synchronize_irq(i) do { } while (0)

/* DMA mapping, identity in userspace */
#define DMA_TO_DEVICE 1
#define DMA_FROM_DEVICE 2
#define DMA_BIDIRECTIONAL 0
#define dma_map_single(d, p, s, dir) ((dma_addr_t) (uintptr_t) (p))
#define dma_unmap_single(d, a, s, dir) do { } while (0)
#define dma_mapping_error(d, a) 0
#define dma_sync_single_for_cpu(d, a, s, dir) do { } while (0)
#define dma_sync_single_for_device(d, a, s, dir) do { } while (0)

/* idr: small integer ids for pointers */
struct idr {
    void **ptrs;
    int size;
    int next;
};
#define DEFINE_IDR(name) struct idr name = { NULL, 0, 0 }
static inline void idr_init(struct idr *idr) { idr->ptrs = NULL; idr->size = 0; idr->next = 0; }
int idr_alloc(struct idr *idr, void *ptr, int start, int end, gfp_t gfp);
void *idr_find(const struct idr *idr, unsigned long id);
void *idr_remove(struct idr *idr, unsigned long id);
void *idr_replace(struct idr *idr, void *ptr, unsigned long id);
void idr_destroy(struct idr *idr);
bool idr_is_empty(const struct idr *idr);
void *idr_get_next(const struct idr *idr, int *nextid);
#define idr_for_each_entry(idr, entry, id) \
    for ((id) = 0; ((entry) = idr_get_next(idr, &(id))) != NULL; (id) += 1U)

/* Polling, as linux/iopoll.h */
#define read_poll_timeout(op, val, cond, sleep_us, timeout_us, sleep_before, args...) ({ \
    u64 __deadline = (u64) ktime_get() + (u64) (timeout_us) * 1000; \
    int __ret = 0; \
    if (sleep_before && (sleep_us)) usleep_range(sleep_us, sleep_us); \
    for (;;) { \
        (val) = op(args); \
        if (cond) break; \
        if ((timeout_us) && (u64) ktime_get() > __deadline) { \
            (val) = op(args); __ret = (cond) ? 0 : -ETIMEDOUT; break; } \
        if (sleep_us) usleep_range(sleep_us, sleep_us); \
    } \
    __ret; })
#define read_poll_timeout_atomic read_poll_timeout
#define readx_poll_timeout(op, addr, val, cond, sleep_us, timeout_us) \
    read_poll_timeout(op, val, cond, sleep_us, timeout_us, false, addr)

/* Tracepoints compile to nothing */
#define TP_PROTO(args...) args
#define TP_ARGS(args...) args
#define TP_STRUCT__entry(args...)
#define TP_fast_assign(args...)
#define TP_printk(args...)
#define TRACE_EVENT(name, proto, args, tstruct, assign, print) \
    static inline void trace_##name(proto) { }
#define DECLARE_EVENT_CLASS(name, proto, args, tstruct, assign, print)
#define DEFINE_EVENT(template, name, proto, args) \
    static inline void trace_##name(proto) { }

/* Firmware loading */
struct firmware {
    size_t size;
    const u8 *data;
};
int request_firmware(const struct firmware **fw, const char *name, struct device *dev);
#define request_firmware_direct request_firmware
int firmware_request_nowarn(const struct firmware **fw, const char *name, struct device *dev);
/* Loads synchronously, then calls cont from the caller's context */
int request_firmware_nowait(struct module *module, bool uevent, const char *name, struct device *dev,
        gfp_t gfp, void *context, void (*cont)(const struct firmware *fw, void *context));
void release_firmware(const struct firmware *fw);
/* Directories searched in order; set by the host program, and followed on Linux by the system
 * firmware directories in the kernel's search order */
void kc_firmware_set_paths(const char *const *paths, size_t n);
enum kc_fw_dirs {
    KC_FW_DIRS_ALL,
    KC_FW_DIRS_HOST,
    KC_FW_DIRS_SYSTEM,
};
size_t kc_firmware_get_paths(enum kc_fw_dirs which, const char *const **paths);
/* Quietly, from only some of the directories, the first of several names found; each
 * directory is searched for all the names before the next */
int kc_firmware_request_in(const struct firmware **fw, const char *const *names, size_t num_names,
        struct device *dev, enum kc_fw_dirs which);
/* Firmware and board config file names the user chose, for drivers that take them; NULL for
 * the driver's own choice */
void kc_firmware_set_choice(const char *firmware, const char *boardfile);
const char *kc_firmware_choice(void);
const char *kc_firmware_board_choice(void);
/* Regulatory country the user chose, for drivers that fix it when firmware loads; NULL for
 * the driver's own */
void kc_set_country_choice(const char *alpha2);
const char *kc_country_choice(void);
/* Sees each firmware file as it is read, and may replace it (freeing the old data) or
 * refuse it with a negative errno */
typedef int (*kc_fw_filter_t)(const char *name, uint8_t **data, size_t *len);
void kc_firmware_set_filter(kc_fw_filter_t filter);
/* Sets a registered module parameter; -ENOENT if no driver has it */
int kc_param_set(const char *name, long value);

/* Misc */
#define kallsyms_lookup_name(n) 0
#define rfkill_blocked(r) false
#define in_interrupt() 0
#define in_softirq() 0
#define in_task() 1
#define num_online_cpus() 1
#define nr_cpu_ids 1
#define get_random_u32() ((u32) random())
#define get_random_bytes(buf, len) kc_get_random_bytes(buf, len)
void kc_get_random_bytes(void *buf, size_t len);
#define prandom_u32_max(n) (get_random_u32() % (n))
#define get_random_u32_below(n) (get_random_u32() % (n))

#endif
