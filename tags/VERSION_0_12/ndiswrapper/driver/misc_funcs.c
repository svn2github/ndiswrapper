/*
 *  Copyright (C) 2003-2004 Pontus Fuchs, Giridhar Pemmasani
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 */
#include <linux/types.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <asm/io.h>
#include <linux/ctype.h>
#include <linux/net.h>

#include "ndis.h"
#include "wrapper.h"

struct list_head wrap_allocs;
struct wrap_spinlock wrap_allocs_lock;

void *wrap_kmalloc(size_t size, int flags)
{
	struct wrap_alloc *alloc;
	TRACEENTER4("size = %d, flags = %d", size, flags);
	if ((flags & GFP_ATOMIC) || irqs_disabled())
		alloc = kmalloc(sizeof(*alloc), GFP_ATOMIC);
	else
		alloc = kmalloc(sizeof(*alloc), GFP_KERNEL);
	if (!alloc)
		return NULL;
	alloc->ptr = kmalloc(size, flags);
	if (!alloc->ptr)
	{
		kfree(alloc);
		return NULL;
	}
	wrap_spin_lock(&wrap_allocs_lock);
	list_add(&alloc->list, &wrap_allocs);
	wrap_spin_unlock(&wrap_allocs_lock);
	DBGTRACE4("%p, %p", alloc, alloc->ptr);
	TRACEEXIT4(return alloc->ptr);
}

void wrap_kfree(void *ptr)
{
	struct list_head *cur, *tmp;

	TRACEENTER4("%p", ptr);
	wrap_spin_lock(&wrap_allocs_lock);
	list_for_each_safe(cur, tmp, &wrap_allocs)
	{
		struct wrap_alloc *alloc = (struct wrap_alloc *)cur;
		if (alloc->ptr == ptr)
		{
			list_del(&alloc->list);
			kfree(alloc->ptr);
			kfree(alloc);
			break;
		}
	}

	wrap_spin_unlock(&wrap_allocs_lock);
	TRACEEXIT4(return);
}

void wrap_kfree_all(void)
{
	struct list_head *cur, *tmp;

	TRACEENTER4("%s", "");
	wrap_spin_lock(&wrap_allocs_lock);
	list_for_each_safe(cur, tmp, &wrap_allocs)
	{
		struct wrap_alloc *alloc = (struct wrap_alloc *)cur;

		list_del(&alloc->list);
		kfree(alloc->ptr);
		kfree(alloc);

	}

	wrap_spin_unlock(&wrap_allocs_lock);
	TRACEEXIT4(return);
}

void wrapper_timer_handler(unsigned long data)
{
	struct wrapper_timer *timer = (struct wrapper_timer *)data;
	struct kdpc *kdpc;
	STDCALL void (*miniport_timer)(void *specific1, void *ctx,
				       void *specific2, void *specific3);

	TRACEENTER5("%p", timer);
#ifdef DEBUG_TIMER
	BUG_ON(timer == NULL);
	BUG_ON(timer->wrapper_timer_magic != WRAPPER_TIMER_MAGIC);
	BUG_ON(timer->kdpc == NULL);
#endif

	/* don't add the timer if aperiodic; see wrapper_cancel_timer
	 * protect access to kdpc, repeat, and active via spinlock */
	spin_lock(&timer->lock);
	kdpc = timer->kdpc;
	if (timer->repeat) {
		timer->timer.expires = jiffies + timer->repeat;
		add_timer(&timer->timer);
	} else
		timer->active = 0;
	spin_unlock(&timer->lock);

	miniport_timer = kdpc->func;

	/* call the handler after restarting in case it cancels itself */
	if (miniport_timer)
		miniport_timer(kdpc, kdpc->ctx, kdpc->arg1, kdpc->arg2);

	TRACEEXIT5(return);
}

void wrapper_init_timer(struct ktimer *ktimer, void *handle)
{
	struct wrapper_timer *wrapper_timer;
	struct ndis_handle *ndis_handle = (struct ndis_handle *)handle;

	TRACEENTER5("%s", "");
	wrapper_timer = wrap_kmalloc(sizeof(struct wrapper_timer), GFP_ATOMIC);
	if (!wrapper_timer) {
		ERROR("%s", "Cannot malloc mem for timer");
		return;
	}

	memset(wrapper_timer, 27, sizeof(*wrapper_timer));
	init_timer(&wrapper_timer->timer);
	wrapper_timer->timer.data = (unsigned long)wrapper_timer;
	wrapper_timer->timer.function = &wrapper_timer_handler;
	wrapper_timer->active = 0;
	wrapper_timer->repeat = 0;
	wrapper_timer->kdpc = NULL;
#ifdef DEBUG_TIMER
	wrapper_timer->wrapper_timer_magic = WRAPPER_TIMER_MAGIC;
#endif
	ktimer->wrapper_timer = wrapper_timer;
	spin_lock_init(&wrapper_timer->lock);
	if (handle) {
		spin_lock_bh(&ndis_handle->timers_lock);
		list_add(&wrapper_timer->list, &ndis_handle->timers);
		spin_unlock_bh(&ndis_handle->timers_lock);
	}

	DBGTRACE4("added timer %p, wrapper_timer->list %p\n",
		  wrapper_timer, &wrapper_timer->list);
	TRACEEXIT5(return);
}

int wrapper_set_timer(struct wrapper_timer *timer,
                      unsigned long expires, unsigned long repeat,
		      struct kdpc *kdpc)
{
	TRACEENTER5("%p", timer);
	if (!timer) {
		ERROR("%s", "invalid timer");
		return 0;
	}

#ifdef DEBUG_TIMER
	if (timer->wrapper_timer_magic != WRAPPER_TIMER_MAGIC) {
		WARNING("timer %p is not initialized (%lu)",
			timer, timer->wrapper_timer_magic);
		timer->wrapper_timer_magic = WRAPPER_TIMER_MAGIC;
	}
#endif

	/* timer handler also uses timer->repeat, active, and kdpc, so
	 * protect in case of SMP */
	spin_lock_bh(&timer->lock);
	if (kdpc)
		timer->kdpc = kdpc;
	timer->repeat = repeat;
	if (timer->active) {
		DBGTRACE4("modifying timer %p to %lu, %lu",
			  timer, expires, repeat);
		mod_timer(&timer->timer, expires);
		spin_unlock_bh(&timer->lock);
		TRACEEXIT5(return 1);
	} else {
		DBGTRACE4("setting timer %p to %lu, %lu",
			  timer, expires, repeat);
		timer->timer.expires = expires;
		timer->active = 1;
		add_timer(&timer->timer);
		spin_unlock_bh(&timer->lock);
		TRACEEXIT5(return 0);
	}
}

void wrapper_cancel_timer(struct wrapper_timer *timer, char *canceled)
{
	TRACEENTER4("timer = %p, canceled = %p", timer, canceled);
	if (!timer) {
		ERROR("%s", "invalid timer");
		return;
	}

#ifdef DEBUG_TIMER
	DBGTRACE4("canceling timer %p", timer);
	BUG_ON(timer->wrapper_timer_magic != WRAPPER_TIMER_MAGIC);
#endif
	/* timer handler also uses timer->repeat, so protect it in
	 * case of SMP */

	/* Oh, and del_timer_sync is not required ('canceled' argument
	 * tells the driver if the timer was deleted or not) here; nor
	 * is del_timer_sync correct, as this function may be called
	 * at DISPATCH_LEVEL */
	spin_lock_bh(&timer->lock);
	if (timer->repeat) {
		/* first mark as aperiodic, so timer function doesn't call
		 * add_timer after del_timer returned */
		timer->repeat = 0;
		del_timer(&timer->timer);
		/* periodic timers always return TRUE */
		*canceled = TRUE;
	} else
		*canceled = del_timer(&timer->timer);
	spin_unlock_bh(&timer->lock);
	TRACEEXIT5(return);
}

NOREGPARM int WRAP_EXPORT(_wrap_sprintf)
	(char *buf, const char *format, ...)
{
	va_list args;
	int res;
	va_start(args, format);
	res = vsprintf(buf, format, args);
	va_end(args);
	return res;
}

NOREGPARM int WRAP_EXPORT(_wrap_vsprintf)
	(char *str, const char *format, va_list ap)
{
	return vsprintf(str, format, ap);
}

NOREGPARM int WRAP_EXPORT(_wrap_snprintf)
	(char *buf, size_t count, const char *format, ...)
{
	va_list args;
	int res;

	va_start(args, format);
	res = vsnprintf(buf, count, format, args);
	va_end(args);
	return res;
}

NOREGPARM int WRAP_EXPORT(_wrap__snprintf)
	(char *buf, size_t count, const char *format, ...)
{
	va_list args;
	int res;

	va_start(args, format);
	res = vsnprintf(buf, count, format, args);
	va_end(args);
	return res;
}

NOREGPARM int WRAP_EXPORT(_wrap_vsnprintf)
	(char *str, size_t size, const char *format,
			  va_list ap)
{
	return vsnprintf(str, size, format, ap);
}

NOREGPARM int WRAP_EXPORT(_wrap__vsnprintf)
	(char *str, size_t size, const char *format,
			    va_list ap)
{
	return vsnprintf(str, size, format, ap);
}

NOREGPARM char * WRAP_EXPORT(_wrap_strncpy)
	(char *dst, char *src, int n)
{
	return strncpy(dst, src, n);
}

NOREGPARM size_t WRAP_EXPORT(_wrap_strlen)
	(const char *s)
{
       return strlen(s);
}

NOREGPARM int WRAP_EXPORT(_wrap_strncmp)
	(const char *s1, const char *s2, size_t n)
{
	return strncmp(s1, s2, n);
}

NOREGPARM int WRAP_EXPORT(_wrap_strcmp)
	(const char *s1, const char *s2)
{
	return strcmp(s1, s2);
}

int stricmp(const char *s1, const char *s2)
{
	while (*s1 && *s2 && tolower(*s1) == tolower(*s2))
	{
		s1++;
		s2++;
	}
	return (int)*s1 - (int)*s2;
}

NOREGPARM int WRAP_EXPORT(_wrap_tolower)
	(int c)
{
	return tolower(c);
}

NOREGPARM int WRAP_EXPORT(_wrap_toupper)
	(int c)
{
	return toupper(c);
}

NOREGPARM void *WRAP_EXPORT(_wrap_memcpy)
	(void * to, const void * from, size_t n)
{
	return memcpy(to, from, n);
}

NOREGPARM void *WRAP_EXPORT(_wrap_strcpy)
	(void * to, const void * from)
{
	return strcpy(to, from);
}

NOREGPARM void *WRAP_EXPORT(_wrap_memset)
	(void * s, char c,size_t count)
{
	return memset(s, c, count);
}

NOREGPARM void *WRAP_EXPORT(_wrap_memmove)
	(void *to, void *from, size_t count)
{
	return memmove(to, from, count);
}

NOREGPARM void WRAP_EXPORT(_wrap_srand)
	(unsigned int seed)
{
	net_srandom(seed);
}

NOREGPARM int WRAP_EXPORT(_wrap_atoi)
	(const char *ptr)
{
	int i = simple_strtol(ptr, NULL, 10);
	return i;
}

STDCALL __s64 WRAP_EXPORT(_alldiv)
	(__s64 a, __s64 b)
{
	return (a / b);
}

STDCALL __u64 WRAP_EXPORT(_aulldiv)
	(__u64 a, __u64 b)
{
	return (a / b);
}

STDCALL __s64 WRAP_EXPORT(_allmul)
	(__s64 a, __s64 b)
{
	return (a * b);
}

STDCALL __u64 WRAP_EXPORT(_aullmul)
	(__u64 a, __u64 b)
{
	return (a * b);
}

STDCALL __s64 WRAP_EXPORT(_allrem)
	(__s64 a, __s64 b)
{
	return (a % b);
}

STDCALL __u64 WRAP_EXPORT(_aullrem)
	(__u64 a, __u64 b)
{
	return (a % b);
}

__attribute__ ((regparm(3))) __s64 WRAP_EXPORT(_allshl)
	(__s64 a, __u8 b)
{
	return (a << b);
}

__attribute__ ((regparm(3))) __u64 WRAP_EXPORT(_aullshl)
	(__u64 a, __u8 b)
{
	return (a << b);
}

__attribute__ ((regparm(3))) __s64 WRAP_EXPORT(_allshr)
	(__s64 a, __u8 b)
{
	return (a >> b);
}

__attribute__ ((regparm(3))) __u64 WRAP_EXPORT(_aullshr)
	(__u64 a, __u8 b)
{
	return (a >> b);
}

STDCALL size_t WRAP_EXPORT(RtlCompareMemory)
	(const void *a, const void *b, size_t len)
{
	size_t i;
	char *x, *y;

	TRACEENTER1("%s", "");

	x = (char *)a;
	y = (char *)b;
	/* MSDN says this should return number of bytes that compare as
	 * equal. This can be interpretted as either all bytes that are
	 * equal in 'len' bytes or that only until the bytes compare as
	 * not equal. Initially we had it the former way, but Realtek driver
	 * doesn't like it that way - it takes many attempts to associate
	 * with WPA. ReactOS returns the number of bytes that are equal
	 * upto when they compare as not equal.
	 * According to lords at #reactos, that is the way it should be
	 * and that msdn is wrong about it!
	 */
	for (i = 0; i < len && x[i] == y[i]; i++)
		;
	return i;
}

STDCALL long WRAP_EXPORT(RtlCompareString)
	(const struct ustring *s1, const struct ustring *s2,
	 int case_insensitive)
{
	unsigned int len;
	long ret = 0;
	const char *p1, *p2;

	TRACEENTER1("%s", "");
	len = min(s1->len, s2->len);
	p1 = s1->buf;
	p2 = s2->buf;

	if (case_insensitive)
		while (!ret && len--)
			ret = toupper(*p1++) - toupper(*p2++);
	else
		while (!ret && len--)
			ret = *p1++ - *p2++;
	if (!ret)
		ret = s1->len - s2->len;
	return ret;
}

STDCALL long WRAP_EXPORT(RtlCompareUnicodeString)
	(const struct ustring *s1, const struct ustring *s2,
	 int case_insensitive)
{
	unsigned int len;
	long ret = 0;
	const __u16 *p1, *p2;

	TRACEENTER1("%s", "");
	len = min(s1->len, s2->len);
	p1 = (__u16 *)s1->buf;
	p2 = (__u16 *)s2->buf;

	if (case_insensitive)
		while (!ret && len--)
			ret = toupper((__u8)*p1++) - toupper((__u8)*p2++);
	else
		while (!ret && len--)
			ret = *p1++ - *p2++;
	if (!ret)
		ret = s1->len - s2->len;
	return ret;
}

STDCALL int WRAP_EXPORT(RtlEqualString)
	(const struct ustring *s1, const struct ustring *s2,
	 int case_insensitive)
{
	TRACEENTER1("%s", "");
	if (s1->len != s2->len)
		return 0;
	return !RtlCompareString(s1, s2, case_insensitive);
}

STDCALL int WRAP_EXPORT(RtlEqualUnicodeString)
	(const struct ustring *s1, const struct ustring *s2,
	 int case_insensitive)
{
	if (s1->len != s2->len)
		return 0;
	return !RtlCompareUnicodeString(s1, s2, case_insensitive);
}

STDCALL void WRAP_EXPORT(RtlCopyUnicodeString)
	(struct ustring *dst, const struct ustring *src)
{
	TRACEENTER1("%s", "");
	if (src)
	{
		unsigned int len = min(src->len, dst->buflen);
		memcpy(dst->buf, src->buf, len);
		dst->len = len;
		/* append terminating '\0' if enough space */
		if (len < dst->buflen)
			dst->buf[len] = 0;
	}
	else dst->len = 0;
	TRACEEXIT1(return);
}

STDCALL int WRAP_EXPORT(RtlAnsiStringToUnicodeString)
	(struct ustring *dst, struct ustring *src, unsigned int dup)
{
	int i;
	__u16 *d;
	__u8 *s;

	TRACEENTER2("dup: %d src: %s", dup, src->buf);
	if(dup)
	{
		char *buf = kmalloc((src->buflen+1) * sizeof(__u16),
				    GFP_KERNEL);
		if(!buf)
			TRACEEXIT1(return NDIS_STATUS_FAILURE);
		dst->buf = buf;
		dst->buflen = (src->buflen+1) * sizeof(__u16);
	}
	else if (dst->buflen < (src->len+1) * sizeof(__u16))
		TRACEEXIT1(return NDIS_STATUS_FAILURE);

	dst->len = src->len * sizeof(__u16);
	d = (__u16 *)dst->buf;
	s = (__u8 *)src->buf;
	for(i = 0; i < src->len; i++)
	{
		d[i] = (__u16)s[i];
	}
	d[i] = 0;

	TRACEEXIT2(return NDIS_STATUS_SUCCESS);
}

STDCALL int WRAP_EXPORT(RtlUnicodeStringToAnsiString)
	(struct ustring *dst, struct ustring *src, unsigned int dup)
{
	int i;
	__u16 *s;
	__u8 *d;

	TRACEENTER2("dup: %d src->len: %d src->buflen: %d, dst: %p",
		    dup, src->len, src->buflen, dst);
	if(dup)
	{
		char *buf = kmalloc((src->buflen+1) / sizeof(__u16),
				    GFP_KERNEL);
		if(!buf)
			return NDIS_STATUS_FAILURE;
		dst->buf = buf;
		dst->buflen = (src->buflen+1) / sizeof(__u16);
	}
	else if (dst->buflen < (src->len+1) / sizeof(__u16))
		return NDIS_STATUS_FAILURE;

	dst->len = src->len / sizeof(__u16);
	s = (__u16 *)src->buf;
	d = (__u8 *)dst->buf;
	for(i = 0; i < dst->len; i++)
		d[i] = (__u8)s[i];
	d[i] = 0;

	TRACEEXIT2(return NDIS_STATUS_SUCCESS);
}

STDCALL int WRAP_EXPORT(RtlIntegerToUnicodeString)
	(unsigned long value, unsigned long base, struct ustring *ustring)
{
	char string[sizeof(unsigned long) * 8 + 1];
	struct ustring ansi;
	int i;

	TRACEENTER1("%s", "");
	if (base == 0)
		base = 10;
	if (!(base == 2 || base == 8 || base == 10 || base == 16))
		return NDIS_STATUS_INVALID_PARAMETER;
	for (i = 0; value && i < sizeof(string); i++)
	{
		int r;
		r = value % base;
		value /= base;
		if (r < 10)
			string[i] = r + '0';
		else
			string[i] = r + 'a' - 10;
	}

	if (i < sizeof(string))
		string[i] = 0;
	else
		return NDIS_STATUS_BUFFER_TOO_SHORT;

	ansi.buf = string;
	ansi.len = strlen(string);
	ansi.buflen = sizeof(string);
	return RtlAnsiStringToUnicodeString(ustring, &ansi, 0);
}

STDCALL void  WRAP_EXPORT(RtlInitUnicodeString)
	(struct ustring *dest, __u16 *src)
{
	struct ustring *uc;

	TRACEENTER1("%s", "");
	uc = dest;
	if (uc == NULL)
		TRACEEXIT1(return);
	if (src == NULL) {
		dest->len = dest->buflen = 0;
		dest->buf = NULL;
	} else {
		int i = 0;
		while (src[i])
			i++;
		dest->buf = (char *)src;
		dest->len = dest->buflen = i * 2;
	}
	TRACEEXIT1(return);
}

static void WRAP_EXPORT(RtlFreeUnicodeString)(void){UNIMPL();}
static void WRAP_EXPORT(RtlUnwind)(void){UNIMPL();}
STDCALL static unsigned int WRAP_EXPORT(RtlQueryRegistryValues)
	(unsigned long relative, char* path, void* tbl,
	 void* context, void* env)
{
	TRACEENTER5("%s", "");
	UNIMPL();
	TRACEEXIT5(return STATUS_SUCCESS);
}


STDCALL int WRAP_EXPORT(rand)
	(void)
{
	char buf[6];
	int i, r;

	get_random_bytes(buf, sizeof(buf));
	for (r = i = 0; i < sizeof(buf) ; i++)
		r += buf[i];
	return r;
}

/*
 * This function gets scheduled from NdisMIndicateReceivePacket
 */
void packet_recycler(void *param)
{
	struct ndis_handle *handle = (struct ndis_handle*) param;
	KIRQL irql;

	TRACEENTER3("%s", "Packet recycler running");
	while (1)
	{
		struct ndis_packet * packet = NULL;
		struct miniport_char *miniport;
		miniport = &handle->driver->miniport_char;

		wrap_spin_lock(&handle->recycle_packets_lock);
		if (list_empty(&handle->recycle_packets))
		{
			wrap_spin_unlock(&handle->recycle_packets_lock);
			break;
		}
		else
		{
			packet = (struct ndis_packet*)
				handle->recycle_packets.next;

			list_del(handle->recycle_packets.next);
			DBGTRACE3("Picking packet at %p!", packet);
			packet = (struct ndis_packet*)
				((char*)packet -
				 ((char*) &packet->recycle_list -
				  (char*) &packet->nr_pages));
		}

		wrap_spin_unlock(&handle->recycle_packets_lock);

		if (packet == NULL)
			break;

		packet->status = NDIS_STATUS_SUCCESS;
		irql = raise_irql(DISPATCH_LEVEL);
		miniport->return_packet(handle->adapter_ctx, packet);
		lower_irql(irql);
	}
	TRACEEXIT3(return);
}

u64 ticks_1601(void)
{
	struct timeval now;
	u64 ticks;

	do_gettimeofday(&now);
	ticks = (u64) now.tv_sec * TICKSPERSEC;
	ticks += now.tv_usec * 10 + TICKS_1601_TO_1970;
	return ticks;
}

int get_esp(void)
{
	volatile int i;
	asm("movl %esp,(%esp,1)");
	return i;
}

void inline my_dumpstack(void)
{
	int *sp = (int*) get_esp();
	int i;
	for(i = 0; i < 20; i++)
	{
		printk("%08x\n", sp[i]);
	}
}

#include "misc_funcs_exports.h"
