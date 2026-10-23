#ifndef MX_BITS_H
#define MX_BITS_H

/* Field access for the generated names in <mx/hw/regs.h> and
   <mx/hw/formats.h>: MX_SET(DMATAG_QWC, n) places n in the QWC field of a
   DMA tag, MX_GET(DMATAG_QWC, tag) extracts it. */
#define MX_SET(F, v) (((unsigned long long)(v) & MX_##F##_MASK) << MX_##F##_SHIFT)
#define MX_GET(F, x) (((unsigned long long)(x) >> MX_##F##_SHIFT) & MX_##F##_MASK)
#define MX_BIT(F) (MX_##F##_MASK << MX_##F##_SHIFT)

#endif
