# Hardware description of the PS2 Emotion Engine, as data.
#
# gen.py turns this into include/mx/hw/*.h, src/base/syscalls.S and
# tests/host/test_gen.c. Field positions are from the EE User's Manual (DMAC
# ch. 5, VIF ch. 6, GIF ch. 7, INTC/timers ch. 4, 10), the GS User's Manual
# and the VU User's Manual. Each field is (name, lsb, width).

# ---------------------------------------------------------------------------
# Single registers: (name, address, width, fields)

REGS = [
    # DMAC common registers
    ("D_CTRL", 0x1000E000, 32, [
        ("DMAE", 0, 1), ("RELE", 1, 1), ("MFD", 2, 2), ("STS", 4, 2),
        ("STD", 6, 2), ("RCYC", 8, 3)]),
    ("D_STAT", 0x1000E010, 32, [
        ("CIS", 0, 10), ("SIS", 13, 1), ("MEIS", 14, 1), ("BEIS", 15, 1),
        ("CIM", 16, 10), ("SIM", 29, 1), ("MEIM", 30, 1)]),
    ("D_PCR", 0x1000E020, 32, [("CPC", 0, 10), ("CDE", 16, 10), ("PCE", 31, 1)]),
    ("D_SQWC", 0x1000E030, 32, [("SQWC", 0, 8), ("TQWC", 16, 8)]),
    ("D_RBSR", 0x1000E040, 32, [("RMSK", 4, 27)]),
    ("D_RBOR", 0x1000E050, 32, [("ADDR", 0, 31)]),
    ("D_STADR", 0x1000E060, 32, [("ADDR", 0, 31)]),
    ("D_ENABLER", 0x1000F520, 32, [("CPND", 16, 1)]),
    ("D_ENABLEW", 0x1000F590, 32, [("CPND", 16, 1)]),

    # INTC
    ("I_STAT", 0x1000F000, 32, [("CAUSE", 0, 15)]),
    ("I_MASK", 0x1000F010, 32, [("CAUSE", 0, 15)]),

    # VIF0 / VIF1 control registers that are not shared (see VIF block below)
    # GIF
    ("GIF_CTRL", 0x10003000, 32, [("RST", 0, 1), ("PSE", 3, 1)]),
    ("GIF_MODE", 0x10003010, 32, [("M3R", 0, 1), ("IMT", 2, 1)]),
    ("GIF_STAT", 0x10003020, 32, [
        ("M3R", 0, 1), ("M3P", 1, 1), ("IMT", 2, 1), ("PSE", 3, 1), ("IP3", 5, 1),
        ("P3Q", 6, 1), ("P2Q", 7, 1), ("P1Q", 8, 1), ("OPH", 9, 1), ("APATH", 10, 2),
        ("DIR", 12, 1), ("FQC", 24, 5)]),

    # GS privileged registers (64 bit)
    ("GS_PMODE", 0x12000000, 64, [
        ("EN1", 0, 1), ("EN2", 1, 1), ("CRTMD", 2, 3), ("MMOD", 5, 1), ("AMOD", 6, 1),
        ("SLBG", 7, 1), ("ALP", 8, 8)]),
    ("GS_SMODE1", 0x12000010, 64, []),
    ("GS_SMODE2", 0x12000020, 64, [("INT", 0, 1), ("FFMD", 1, 1), ("DPMS", 2, 2)]),
    ("GS_DISPFB1", 0x12000070, 64, [
        ("FBP", 0, 9), ("FBW", 9, 6), ("PSM", 15, 5), ("DBX", 32, 11), ("DBY", 43, 11)]),
    ("GS_DISPLAY1", 0x12000080, 64, [
        ("DX", 0, 12), ("DY", 12, 11), ("MAGH", 23, 4), ("MAGV", 27, 2),
        ("DW", 32, 12), ("DH", 44, 11)]),
    ("GS_DISPFB2", 0x12000090, 64, [
        ("FBP", 0, 9), ("FBW", 9, 6), ("PSM", 15, 5), ("DBX", 32, 11), ("DBY", 43, 11)]),
    ("GS_DISPLAY2", 0x120000A0, 64, [
        ("DX", 0, 12), ("DY", 12, 11), ("MAGH", 23, 4), ("MAGV", 27, 2),
        ("DW", 32, 12), ("DH", 44, 11)]),
    ("GS_BGCOLOR", 0x120000E0, 64, [("R", 0, 8), ("G", 8, 8), ("B", 16, 8)]),
    ("GS_CSR", 0x12001000, 64, [
        ("SIGNAL", 0, 1), ("FINISH", 1, 1), ("HSINT", 2, 1), ("VSINT", 3, 1),
        ("EDWINT", 4, 1), ("FLUSH", 8, 1), ("RESET", 9, 1), ("NFIELD", 12, 1),
        ("FIELD", 13, 1), ("FIFO", 14, 2), ("REV", 16, 8), ("ID", 24, 8)]),
    ("GS_IMR", 0x12001010, 64, [
        ("SIGMSK", 8, 1), ("FINISHMSK", 9, 1), ("HSMSK", 10, 1), ("VSMSK", 11, 1),
        ("EDWMSK", 12, 1)]),
    ("GS_BUSDIR", 0x12001040, 64, [("DIR", 0, 1)]),
    ("GS_SIGLBLID", 0x12001080, 64, [("SIGID", 0, 32), ("LBLID", 32, 32)]),
]

# ---------------------------------------------------------------------------
# Register blocks repeated per unit: (block, {unit: base}, [(reg, offset, fields)])

DMA_CHANNELS = {
    "VIF0": 0x10008000, "VIF1": 0x10009000, "GIF": 0x1000A000,
    "FROMIPU": 0x1000B000, "TOIPU": 0x1000B400, "SIF0": 0x1000C000,
    "SIF1": 0x1000C400, "SIF2": 0x1000C800, "FROMSPR": 0x1000D000,
    "TOSPR": 0x1000D400,
}

BLOCKS = [
    ("DMA", DMA_CHANNELS, [
        ("CHCR", 0x00, [("DIR", 0, 1), ("MOD", 2, 2), ("ASP", 4, 2), ("TTE", 6, 1),
                        ("TIE", 7, 1), ("STR", 8, 1), ("TAG", 16, 16)]),
        ("MADR", 0x10, [("ADDR", 0, 31), ("SPR", 31, 1)]),
        ("QWC", 0x20, [("QWC", 0, 16)]),
        ("TADR", 0x30, [("ADDR", 0, 31), ("SPR", 31, 1)]),
        ("ASR0", 0x40, [("ADDR", 0, 31), ("SPR", 31, 1)]),
        ("ASR1", 0x50, [("ADDR", 0, 31), ("SPR", 31, 1)]),
        ("SADR", 0x80, [("ADDR", 0, 14)]),
    ]),
    ("VIF", {"VIF0": 0x10003800, "VIF1": 0x10003C00}, [
        ("STAT", 0x00, [("VPS", 0, 2), ("VEW", 2, 1), ("VGW", 3, 1), ("MRK", 6, 1),
                        ("DBF", 7, 1), ("VSS", 8, 1), ("VFS", 9, 1), ("VIS", 10, 1),
                        ("INT", 11, 1), ("ER0", 12, 1), ("ER1", 13, 1), ("FDR", 23, 1),
                        ("FQC", 24, 5)]),
        ("FBRST", 0x10, [("RST", 0, 1), ("FBK", 1, 1), ("STP", 2, 1), ("STC", 3, 1)]),
        ("ERR", 0x20, [("MII", 0, 1), ("ME0", 1, 1), ("ME1", 2, 1)]),
        ("MARK", 0x30, [("MARK", 0, 16)]),
        ("CYCLE", 0x40, [("CL", 0, 8), ("WL", 8, 8)]),
        ("MODE", 0x50, [("MOD", 0, 2)]),
        ("NUM", 0x60, [("NUM", 0, 8)]),
        ("MASK", 0x70, []),
        ("CODE", 0x80, []),
        ("ITOPS", 0x90, [("ITOPS", 0, 10)]),
        ("BASE", 0xA0, [("BASE", 0, 10)]),
        ("OFST", 0xB0, [("OFFSET", 0, 10)]),
        ("TOPS", 0xC0, [("TOPS", 0, 10)]),
        ("ITOP", 0xD0, [("ITOP", 0, 10)]),
        ("TOP", 0xE0, [("TOP", 0, 10)]),
    ]),
    ("TIMER", {"T0": 0x10000000, "T1": 0x10000800, "T2": 0x10001000, "T3": 0x10001800}, [
        ("COUNT", 0x00, [("COUNT", 0, 16)]),
        ("MODE", 0x10, [("CLKS", 0, 2), ("GATE", 2, 1), ("GATS", 3, 1), ("GATM", 4, 2),
                        ("ZRET", 6, 1), ("CUE", 7, 1), ("CMPE", 8, 1), ("OVFE", 9, 1),
                        ("EQUF", 10, 1), ("OVFF", 11, 1)]),
        ("COMP", 0x20, [("COMP", 0, 16)]),
        ("HOLD", 0x30, [("HOLD", 0, 16)]),
    ]),
]

# ---------------------------------------------------------------------------
# Packed values that are built in memory (tags, codes, GS register data):
# (name, width, fields). Fields of 128-bit formats are on the low 64 bits.

FORMATS = [
    # DMA source chain tag (low 64 bits; the high 64 bits carry two VIF codes)
    ("DMATAG", 64, [("QWC", 0, 16), ("PCE", 26, 2), ("ID", 28, 3), ("IRQ", 31, 1),
                    ("ADDR", 32, 31), ("SPR", 63, 1)]),
    # VIF code
    ("VIFCODE", 32, [("IMM", 0, 16), ("NUM", 16, 8), ("CMD", 24, 7), ("I", 31, 1)]),
    # UNPACK immediate
    ("UNPACK_IMM", 16, [("ADDR", 0, 10), ("USN", 14, 1), ("FLG", 15, 1)]),
    # GIF tag (low 64 bits; the high 64 bits are REGS, 16 x 4 bits)
    ("GIFTAG", 64, [("NLOOP", 0, 15), ("EOP", 15, 1), ("PRE", 46, 1), ("PRIM", 47, 11),
                    ("FLG", 58, 2), ("NREG", 60, 4)]),
    # COP2 control registers
    ("VPU_STAT", 32, [("VBS0", 0, 1), ("VDS0", 1, 1), ("VTS0", 2, 1), ("VFS0", 3, 1),
                      ("DIV0", 5, 1), ("IBS0", 7, 1), ("VBS1", 8, 1), ("VDS1", 9, 1),
                      ("VTS1", 10, 1), ("VFS1", 11, 1), ("VGW1", 12, 1), ("DIV1", 13, 1),
                      ("EFU1", 14, 1)]),
    ("VU_FBRST", 32, [("FB0", 0, 1), ("RS0", 1, 1), ("DE0", 2, 1), ("TE0", 3, 1),
                      ("FB1", 8, 1), ("RS1", 9, 1), ("DE1", 10, 1), ("TE1", 11, 1)]),
    # COP0 performance counter control
    ("PCCR", 32, [("EXL0", 1, 1), ("K0", 2, 1), ("S0", 3, 1), ("U0", 4, 1),
                  ("EVENT0", 5, 5), ("EXL1", 11, 1), ("K1", 12, 1), ("S1", 13, 1),
                  ("U1", 14, 1), ("EVENT1", 15, 5), ("CTE", 31, 1)]),

    # GS general registers (64-bit data written through A+D or GIF descriptors)
    ("GS_PRIM", 64, [("PRIM", 0, 3), ("IIP", 3, 1), ("TME", 4, 1), ("FGE", 5, 1),
                     ("ABE", 6, 1), ("AA1", 7, 1), ("FST", 8, 1), ("CTXT", 9, 1),
                     ("FIX", 10, 1)]),
    ("GS_RGBAQ", 64, [("R", 0, 8), ("G", 8, 8), ("B", 16, 8), ("A", 24, 8), ("Q", 32, 32)]),
    ("GS_ST", 64, [("S", 0, 32), ("T", 32, 32)]),
    ("GS_UV", 64, [("U", 0, 14), ("V", 16, 14)]),
    ("GS_XYZ", 64, [("X", 0, 16), ("Y", 16, 16), ("Z", 32, 32)]),
    ("GS_XYZF", 64, [("X", 0, 16), ("Y", 16, 16), ("Z", 32, 24), ("F", 56, 8)]),
    ("GS_TEX0", 64, [("TBP0", 0, 14), ("TBW", 14, 6), ("PSM", 20, 6), ("TW", 26, 4),
                     ("TH", 30, 4), ("TCC", 34, 1), ("TFX", 35, 2), ("CBP", 37, 14),
                     ("CPSM", 51, 4), ("CSM", 55, 1), ("CSA", 56, 5), ("CLD", 61, 3)]),
    ("GS_TEX1", 64, [("LCM", 0, 1), ("MXL", 2, 3), ("MMAG", 5, 1), ("MMIN", 6, 3),
                     ("MTBA", 9, 1), ("L", 19, 2), ("K", 32, 12)]),
    ("GS_CLAMP", 64, [("WMS", 0, 2), ("WMT", 2, 2), ("MINU", 4, 10), ("MAXU", 14, 10),
                      ("MINV", 24, 10), ("MAXV", 34, 10)]),
    ("GS_XYOFFSET", 64, [("OFX", 0, 16), ("OFY", 32, 16)]),
    ("GS_PRMODECONT", 64, [("AC", 0, 1)]),
    ("GS_SCISSOR", 64, [("SCAX0", 0, 11), ("SCAX1", 16, 11), ("SCAY0", 32, 11),
                        ("SCAY1", 48, 11)]),
    ("GS_ALPHA", 64, [("A", 0, 2), ("B", 2, 2), ("C", 4, 2), ("D", 6, 2), ("FIX", 32, 8)]),
    ("GS_TEST", 64, [("ATE", 0, 1), ("ATST", 1, 3), ("AREF", 4, 8), ("AFAIL", 12, 2),
                     ("DATE", 14, 1), ("DATM", 15, 1), ("ZTE", 16, 1), ("ZTST", 17, 2)]),
    ("GS_FRAME", 64, [("FBP", 0, 9), ("FBW", 16, 6), ("PSM", 24, 6), ("FBMSK", 32, 32)]),
    ("GS_ZBUF", 64, [("ZBP", 0, 9), ("PSM", 24, 4), ("ZMSK", 32, 1)]),
    ("GS_BITBLTBUF", 64, [("SBP", 0, 14), ("SBW", 16, 6), ("SPSM", 24, 6), ("DBP", 32, 14),
                          ("DBW", 48, 6), ("DPSM", 56, 6)]),
    ("GS_TRXPOS", 64, [("SSAX", 0, 11), ("SSAY", 16, 11), ("DSAX", 32, 11),
                       ("DSAY", 48, 11), ("DIR", 59, 2)]),
    ("GS_TRXREG", 64, [("RRW", 0, 12), ("RRH", 32, 12)]),
    ("GS_TRXDIR", 64, [("XDIR", 0, 2)]),
    ("GS_SIGNAL", 64, [("ID", 0, 32), ("IDMSK", 32, 32)]),
    ("GS_LABEL", 64, [("ID", 0, 32), ("IDMSK", 32, 32)]),
    ("GS_COLCLAMP", 64, [("CLAMP", 0, 1)]),
    ("GS_TEXA", 64, [("TA0", 0, 8), ("AEM", 15, 1), ("TA1", 32, 8)]),
]

# ---------------------------------------------------------------------------
# Enumerations: (name, {item: value})

ENUMS = [
    ("INTC", {"GS": 0, "SBUS": 1, "VBON": 2, "VBOF": 3, "VIF0": 4, "VIF1": 5, "VU0": 6,
              "VU1": 7, "IPU": 8, "TIM0": 9, "TIM1": 10, "TIM2": 11, "TIM3": 12,
              "SFIFO": 13, "VU0WD": 14}),
    ("DMAC", {"VIF0": 0, "VIF1": 1, "GIF": 2, "FROMIPU": 3, "TOIPU": 4, "SIF0": 5,
              "SIF1": 6, "SIF2": 7, "FROMSPR": 8, "TOSPR": 9, "STALL": 13, "MFIFO": 14,
              "BUSERR": 15}),
    # DMA source chain tag IDs
    ("TAG", {"REFE": 0, "CNT": 1, "NEXT": 2, "REF": 3, "REFS": 4, "CALL": 5, "RET": 6,
             "END": 7}),
    # VIF commands (UNPACK is 0x60 | m << 4 | vn << 2 | vl)
    ("VIF", {"NOP": 0x00, "STCYCL": 0x01, "OFFSET": 0x02, "BASE": 0x03, "ITOP": 0x04,
             "STMOD": 0x05, "MSKPATH3": 0x06, "MARK": 0x07, "FLUSHE": 0x10, "FLUSH": 0x11,
             "FLUSHA": 0x13, "MSCAL": 0x14, "MSCALF": 0x15, "MSCNT": 0x17, "STMASK": 0x20,
             "STROW": 0x30, "STCOL": 0x31, "MPG": 0x4A, "DIRECT": 0x50, "DIRECTHL": 0x51,
             "UNPACK": 0x60}),
    # UNPACK formats (vn << 2 | vl)
    ("UNPACK", {"S_32": 0x0, "S_16": 0x1, "S_8": 0x2, "V2_32": 0x4, "V2_16": 0x5,
                "V2_8": 0x6, "V3_32": 0x8, "V3_16": 0x9, "V3_8": 0xA, "V4_32": 0xC,
                "V4_16": 0xD, "V4_8": 0xE, "V4_5": 0xF}),
    ("GIF_FLG", {"PACKED": 0, "REGLIST": 1, "IMAGE": 2, "DISABLE": 3}),
    # GIF PACKED register descriptors
    ("GIF_REG", {"PRIM": 0x0, "RGBAQ": 0x1, "ST": 0x2, "UV": 0x3, "XYZF2": 0x4,
                 "XYZ2": 0x5, "TEX0_1": 0x6, "TEX0_2": 0x7, "CLAMP_1": 0x8, "CLAMP_2": 0x9,
                 "FOG": 0xA, "XYZF3": 0xC, "XYZ3": 0xD, "AD": 0xE, "NOP": 0xF}),
    # GS general register addresses (A+D)
    ("GS_REG", {"PRIM": 0x00, "RGBAQ": 0x01, "ST": 0x02, "UV": 0x03, "XYZF2": 0x04,
                "XYZ2": 0x05, "TEX0_1": 0x06, "TEX0_2": 0x07, "CLAMP_1": 0x08,
                "CLAMP_2": 0x09, "FOG": 0x0A, "XYZF3": 0x0C, "XYZ3": 0x0D, "TEX1_1": 0x14,
                "TEX1_2": 0x15, "TEX2_1": 0x16, "TEX2_2": 0x17, "XYOFFSET_1": 0x18,
                "XYOFFSET_2": 0x19, "PRMODECONT": 0x1A, "PRMODE": 0x1B, "TEXCLUT": 0x1C,
                "SCANMSK": 0x22, "MIPTBP1_1": 0x34, "MIPTBP1_2": 0x35,
                "MIPTBP2_1": 0x36, "MIPTBP2_2": 0x37, "TEXA": 0x3B, "FOGCOL": 0x3D, "TEXFLUSH": 0x3F,
                "SCISSOR_1": 0x40, "SCISSOR_2": 0x41, "ALPHA_1": 0x42, "ALPHA_2": 0x43,
                "DIMX": 0x44, "DTHE": 0x45, "COLCLAMP": 0x46, "TEST_1": 0x47,
                "TEST_2": 0x48, "PABE": 0x49, "FBA_1": 0x4A, "FBA_2": 0x4B,
                "FRAME_1": 0x4C, "FRAME_2": 0x4D, "ZBUF_1": 0x4E, "ZBUF_2": 0x4F,
                "BITBLTBUF": 0x50, "TRXPOS": 0x51, "TRXREG": 0x52, "TRXDIR": 0x53,
                "HWREG": 0x54, "SIGNAL": 0x60, "FINISH": 0x61, "LABEL": 0x62}),
    ("GS_PRIM", {"POINT": 0, "LINE": 1, "LINESTRIP": 2, "TRIANGLE": 3, "TRISTRIP": 4,
                 "TRIFAN": 5, "SPRITE": 6}),
    ("GS_PSM", {"CT32": 0x00, "CT24": 0x01, "CT16": 0x02, "CT16S": 0x0A, "T8": 0x13,
                "T4": 0x14, "T8H": 0x1B, "T4HL": 0x24, "T4HH": 0x2C, "Z32": 0x30,
                "Z24": 0x31, "Z16": 0x32, "Z16S": 0x3A}),
]

# Fixed addresses that are not registers with fields
ADDRS = [
    ("VIF0_FIFO", 0x10004000), ("VIF1_FIFO", 0x10005000), ("GIF_FIFO", 0x10006000),
    ("VU0_MICRO", 0x11000000), ("VU0_DATA", 0x11004000),
    ("VU1_MICRO", 0x11008000), ("VU1_DATA", 0x1100C000),
    ("SPR", 0x70000000), ("SIO_TXFIFO", 0x1000F180), ("SIO_ISR", 0x1000F130),
]

# ---------------------------------------------------------------------------
# BIOS syscalls used by the SDK: (C name, number, prototype). A negative
# number is an interrupt-context (i*) call. Numbers are the EE kernel's
# syscall table.

SYSCALLS = [
    ("mx_bios_reset_ee", 0x01, "void (int flags)"),
    ("mx_bios_set_gs_crt", 0x02, "void (int interlace, int mode, int ffmd)"),
    ("mx_bios_exit", 0x04, "void (int status) __attribute__((noreturn))"),
    ("mx_bios_set_tlb_handler", 0x0D, "void *(int code, void (*handler)(void))"),
    ("mx_bios_set_common_handler", 0x0E, "void *(int code, void (*handler)(void))"),
    ("mx_bios_add_intc_handler", 0x10, "int (int cause, int (*handler)(int cause, void *arg, void *addr), int next, void *arg)"),
    ("mx_bios_remove_intc_handler", 0x11, "int (int cause, int id)"),
    ("mx_bios_add_dmac_handler", 0x12, "int (int channel, int (*handler)(int channel, void *arg, void *addr), int next, void *arg)"),
    ("mx_bios_remove_dmac_handler", 0x13, "int (int channel, int id)"),
    ("mx_bios_enable_intc", 0x14, "int (int cause)"),
    ("mx_bios_disable_intc", 0x15, "int (int cause)"),
    ("mx_bios_enable_dmac", 0x16, "int (int channel)"),
    ("mx_bios_disable_dmac", 0x17, "int (int channel)"),
    ("mx_bios_ienable_intc", -0x1A, "int (int cause)"),
    ("mx_bios_idisable_intc", -0x1B, "int (int cause)"),
    ("mx_bios_ienable_dmac", -0x1C, "int (int channel)"),
    ("mx_bios_idisable_dmac", -0x1D, "int (int channel)"),
    ("mx_bios_create_thread", 0x20, "int (void *param)"),
    ("mx_bios_delete_thread", 0x21, "int (int id)"),
    ("mx_bios_start_thread", 0x22, "int (int id, void *arg)"),
    ("mx_bios_exit_thread", 0x23, "void (void)"),
    ("mx_bios_terminate_thread", 0x25, "int (int id)"),
    ("mx_bios_change_thread_priority", 0x29, "int (int id, int priority)"),
    ("mx_bios_rotate_thread_ready_queue", 0x2B, "int (int priority)"),
    ("mx_bios_get_thread_id", 0x2F, "int (void)"),
    ("mx_bios_refer_thread_status", 0x30, "int (int id, void *status)"),
    ("mx_bios_sleep_thread", 0x32, "int (void)"),
    ("mx_bios_wakeup_thread", 0x33, "int (int id)"),
    ("mx_bios_iwakeup_thread", -0x34, "int (int id)"),
    ("mx_bios_setup_thread", 0x3C, "void *(void *gp, void *stack, int stack_size, void *args, void (*root)(void))"),
    ("mx_bios_setup_heap", 0x3D, "void *(void *heap, int heap_size)"),
    ("mx_bios_create_sema", 0x40, "int (void *param)"),
    ("mx_bios_delete_sema", 0x41, "int (int id)"),
    ("mx_bios_signal_sema", 0x42, "int (int id)"),
    ("mx_bios_isignal_sema", -0x43, "int (int id)"),
    ("mx_bios_wait_sema", 0x44, "int (int id)"),
    ("mx_bios_poll_sema", 0x45, "int (int id)"),
    ("mx_bios_ipoll_sema", -0x46, "int (int id)"),
    ("mx_bios_enable_intc_handler", 0x5C, "int (int cause)"),
    ("mx_bios_disable_intc_handler", 0x5D, "int (int cause)"),
    ("mx_bios_enable_dmac_handler", 0x5E, "int (int channel)"),
    ("mx_bios_disable_dmac_handler", 0x5F, "int (int channel)"),
    ("mx_bios_flush_cache", 0x64, "void (int op)"),
    ("mx_bios_iflush_cache", -0x68, "void (int op)"),
    ("mx_bios_gs_get_imr", 0x70, "unsigned long long (void)"),
    ("mx_bios_gs_put_imr", 0x71, "unsigned long long (unsigned long long imr)"),
    ("mx_bios_igs_put_imr", -0x71, "unsigned long long (unsigned long long imr)"),
    ("mx_bios_print", 0x75, "void (const char *fmt, ...)"),
    ("mx_bios_sif_dma_stat", 0x76, "int (unsigned id)"),
    ("mx_bios_sif_set_dma", 0x77, "unsigned (void *transfers, int count)"),
    ("mx_bios_isif_set_dma", -0x77, "unsigned (void *transfers, int count)"),
    ("mx_bios_sif_set_dchain", 0x78, "void (void)"),
    ("mx_bios_isif_set_dchain", -0x78, "void (void)"),
    ("mx_bios_sif_set_reg", 0x79, "int (unsigned reg, int value)"),
    ("mx_bios_sif_get_reg", 0x7A, "int (unsigned reg)"),
    ("mx_bios_machine_type", 0x7E, "int (void)"),
    ("mx_bios_get_gs_dxdy_offset", 0x80, "void (int mode, int *dx, int *dy, int *dw, int *dh)"),
    ("mx_bios_get_memory_size", 0x7F, "int (void)"),
]
