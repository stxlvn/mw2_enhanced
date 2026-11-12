#include "natframe.h"

#ifdef _WIN32
#include <windows.h>
#endif

#define AT(p, off) ((uint8_t *)(p) + (off))
#define RD_U32(p, off) (*(uint32_t *)AT(p, off))
#define RD_I32(p, off) (*(int32_t *)AT(p, off))
#define RD_PTR(p, off) (*(void **)AT(p, off))

static void nap(ngfr *r)
{
	if (r->sleep_ms)
		r->sleep_ms(1);
#ifdef _WIN32
	else
		Sleep(1);
#endif
}

void ngfr_retire(ngfr *r)
{
	uint8_t *e = r->exe;
	while (RD_U32(e, NGFR_RETIRED) != RD_U32(e, NGFR_ISSUED)) {
		nat_query *q = ((nat_query **)AT(e, NGFR_RING))[RD_U32(e, NGFR_RETIRED) & 7];
		if (q && nat_query_get(q, NULL) == 1) // still pending: everything after it is too
			return;
		RD_U32(e, NGFR_RETIRED)++;
	}
}

void ngfr_wait_frames(ngfr *r)
{
	uint8_t *e = r->exe;
	for (;;) {
		ngfr_retire(r);
		if ((int)(RD_U32(e, NGFR_FRAME) - RD_U32(e, NGFR_RETIRED)) < 3)
			break;
		nap(r);
	}
	RD_U32(e, NGFR_FRAME)++;
}

nat_query *ngfr_issue_next(ngfr *r, void **slot)
{
	uint8_t *e = r->exe;
	const uint32_t i = RD_U32(e, NGFR_POOL_INDEX);
	nat_query *q = ((nat_query **)AT(e, NGFR_POOL))[i];
	RD_U32(e, NGFR_POOL_INDEX) = (i + 1) & 0x1f;
	if (slot)
		*slot = q;
	if (q)
		nat_query_end(q);
	return q;
}

int ngfr_present_and_issue(ngfr *r)
{
	uint8_t *e = r->exe;
	const int rc = nat_present(r->dev);
	nat_query *q = ngfr_issue_next(r, (void **)AT(e, NGFR_LAST_EVENT));
	((nat_query **)AT(e, NGFR_RING))[RD_U32(e, NGFR_ISSUED) & 7] = q;
	RD_U32(e, NGFR_ISSUED)++;
	void **cur = (void **)RD_PTR(e, 0x4116580); // the index ring in use: it restarts with the next frame
	if (cur)
		*(int32_t *)cur = 0;
	return rc;
}

int ngfr_pending(const ngfr *r, void *slot_value)
{
	(void)r;
	nat_query *q = slot_value;
	return q && nat_query_get(q, NULL) == 1;
}

void ngfr_drain(ngfr *r, nat_query *q)
{
	while (q && nat_query_get(q, NULL) == 1)
		nap(r);
}
