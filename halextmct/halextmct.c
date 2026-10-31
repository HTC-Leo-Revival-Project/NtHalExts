// HAL extension for the Samsung Exynos4 Multi-Core Timer (MCT).
//
// The Cortex-A9 in the Exynos4210 has no ARM generic timer, so the HAL needs
// the MCT instead. Two timers are registered, like halextgit does for the
// generic timer:
//   - the free-running 64-bit global counter (G_CNT) as the counter,
//   - global comparator 0 (G_COMP0, SPI 57 = GSI 89) as the interrupt timer.
// This is what Linux uses on the 4210 for its clocksource and CPU0 clock
// event (arch/arm/mach-exynos/mct.c), so it only covers one processor.
//
// CSRT resource group: VendorId 'SAMS', DeviceId 0x4210.

#include "halext.h"
#include <arm_acle.h>

#define DMB_SY 0xF

// Load test: bugcheck as soon as the HAL hands us our CSRT group, before
// touching the MCT, to prove the extension is found and loaded at all.
// Shows as Error Code 0x4D435421 ("MCT!"), parameters VendorId ('SAMS' =
// 0x534D4153), DeviceId (0x4210), Handle. Off by default; build with -DLOAD_TEST=1.
#ifndef LOAD_TEST
#define LOAD_TEST 0
#endif
#define MCT_LOAD_TEST_BUGCHECK 0x4D435421

#define MCT_PHYS 0x10050000
#define MCT_SIZE 0x1000
#define MCT_FREQUENCY 24000000 // XUSBXTI
#define MCT_G0_GSIV 89

#define MCT_G_CNT_L 0x100
#define MCT_G_CNT_U 0x104
#define MCT_G_COMP0_L 0x200
#define MCT_G_COMP0_U 0x204
#define MCT_G_TCON 0x240
#define MCT_G_INT_CSTAT 0x244
#define MCT_G_INT_ENB 0x248
#define MCT_G_WSTAT 0x24C

#define MCT_G_TCON_COMP0_ENABLE (1 << 0)
#define MCT_G_TCON_COMP0_AUTO_INC (1 << 1)
#define MCT_G_TCON_START (1 << 8)

// G_WSTAT: set once a write to the matching register has been applied.
#define MCT_G_WSTAT_COMP0_L (1 << 0)
#define MCT_G_WSTAT_COMP0_U (1 << 1)
#define MCT_G_WSTAT_TCON (1 << 16)

typedef enum _MCT_TIMER_TYPE {
	MctTimerInvalid,
	MctTimerCounter,
	MctTimerInterrupts
} MCT_TIMER_TYPE;

typedef struct _MCT_INTERNAL_DATA
{
	MCT_TIMER_TYPE Type;
	volatile UCHAR* Base;
	TIMER_MODE Mode;
	ULONGLONG LastCompareValue;
	ULONGLONG PeriodicInterval;
} MCT_INTERNAL_DATA, *PMCT_INTERNAL_DATA;

static ULONG MctRead(PMCT_INTERNAL_DATA TimerData, ULONG Offset) {
	ULONG Value = *(volatile ULONG*)(TimerData->Base + Offset);
	__dmb(DMB_SY);
	return Value;
}

static void MctWriteNoWait(PMCT_INTERNAL_DATA TimerData, ULONG Offset, ULONG Value) {
	__dmb(DMB_SY);
	*(volatile ULONG*)(TimerData->Base + Offset) = Value;
}

// Comparator and TCON writes only take effect once G_WSTAT acknowledges
// them; the bit is cleared by writing it back (kernel exynos4_mct_write()).
static void MctWrite(PMCT_INTERNAL_DATA TimerData, ULONG Offset, ULONG Value, ULONG Wstat) {
	MctWriteNoWait(TimerData, Offset, Value);
	for (ULONG Tries = 0x1000; Tries != 0; Tries--) {
		if ((MctRead(TimerData, MCT_G_WSTAT) & Wstat) != 0) {
			MctWriteNoWait(TimerData, MCT_G_WSTAT, Wstat);
			return;
		}
	}
}

static ULONGLONG MctReadCounter(PMCT_INTERNAL_DATA TimerData) {
	// The low word can carry into the high word between the two reads.
	ULONG Hi, Lo;
	ULONG Hi2 = MctRead(TimerData, MCT_G_CNT_U);
	do {
		Hi = Hi2;
		Lo = MctRead(TimerData, MCT_G_CNT_L);
		Hi2 = MctRead(TimerData, MCT_G_CNT_U);
	} while (Hi != Hi2);
	return ((ULONGLONG)Hi << 32) | Lo;
}

static void MctSetCompareValue(PMCT_INTERNAL_DATA TimerData, ULONGLONG Value) {
	MctWrite(TimerData, MCT_G_COMP0_L, (ULONG)Value, MCT_G_WSTAT_COMP0_L);
	MctWrite(TimerData, MCT_G_COMP0_U, (ULONG)(Value >> 32), MCT_G_WSTAT_COMP0_U);
}

static void MctComparatorStop(PMCT_INTERNAL_DATA TimerData) {
	ULONG Tcon = MctRead(TimerData, MCT_G_TCON);
	Tcon &= ~(MCT_G_TCON_COMP0_ENABLE | MCT_G_TCON_COMP0_AUTO_INC);
	MctWrite(TimerData, MCT_G_TCON, Tcon, MCT_G_WSTAT_TCON);
	MctWriteNoWait(TimerData, MCT_G_INT_ENB, 0);
	MctWriteNoWait(TimerData, MCT_G_INT_CSTAT, 1);
}

NTSTATUS MctInitialize(PMCT_INTERNAL_DATA TimerData) {
	if (TimerData->Base == NULL) {
		PHYSICAL_ADDRESS MctPhys;
		MctPhys.QuadPart = MCT_PHYS;
		TimerData->Base = (volatile UCHAR*)HalMapIoSpace(MctPhys, MCT_SIZE, MmNonCached);
		if (TimerData->Base == NULL) return STATUS_INSUFFICIENT_RESOURCES;
	}

	// The counter gates everything; UEFI leaves it running, but don't rely on it.
	ULONG Tcon = MctRead(TimerData, MCT_G_TCON);
	if ((Tcon & MCT_G_TCON_START) == 0) {
		MctWrite(TimerData, MCT_G_TCON, Tcon | MCT_G_TCON_START, MCT_G_WSTAT_TCON);
	}

	if (TimerData->Type == MctTimerInterrupts) {
		MctComparatorStop(TimerData);
	}
	return STATUS_SUCCESS;
}

ULONGLONG MctQueryCounter(PMCT_INTERNAL_DATA TimerData) {
	return MctReadCounter(TimerData);
}

NTSTATUS MctArmTimer(PMCT_INTERNAL_DATA TimerData, TIMER_MODE Mode, ULONGLONG TickCount) {
	TimerData->PeriodicInterval = TickCount;
	ULONGLONG CompareValue = MctReadCounter(TimerData) + TickCount;
	TimerData->LastCompareValue = CompareValue;
	TimerData->Mode = Mode;
	MctSetCompareValue(TimerData, CompareValue);
	MctWriteNoWait(TimerData, MCT_G_INT_ENB, 1);
	ULONG Tcon = MctRead(TimerData, MCT_G_TCON);
	MctWrite(TimerData, MCT_G_TCON, Tcon | MCT_G_TCON_COMP0_ENABLE, MCT_G_WSTAT_TCON);
	return STATUS_SUCCESS;
}

void MctAcknowledgeInterrupt(PMCT_INTERNAL_DATA TimerData) {
	// CSTAT has to be cleared twice (kernel exynos4_mct_tick_clear()).
	MctWriteNoWait(TimerData, MCT_G_INT_CSTAT, 1);
	MctWriteNoWait(TimerData, MCT_G_INT_CSTAT, 1);

	if (TimerData->Mode != TimerModePseudoPeriodic) return;

	// Same catch-up rule as halextgit: stay on the period grid unless we fell
	// more than ten periods behind, then restart from now.
	ULONGLONG Counter = MctReadCounter(TimerData);
	ULONGLONG PeriodicInterval = TimerData->PeriodicInterval;
	ULONGLONG LastCompareValue = TimerData->LastCompareValue;
	ULONGLONG CompareValue = 0;
	if ( ((10 * PeriodicInterval) + LastCompareValue) >= Counter ) {
		CompareValue = LastCompareValue + PeriodicInterval;
	} else {
		CompareValue = PeriodicInterval + Counter;
	}
	TimerData->LastCompareValue = CompareValue;
	MctSetCompareValue(TimerData, CompareValue);
}

void MctStop(PMCT_INTERNAL_DATA TimerData) {
	MctComparatorStop(TimerData);
	TimerData->Mode = TimerModeInvalid;
}

NTSTATUS
AddResourceGroup (
    __in ULONG Handle,
    __in PCSRT_RESOURCE_GROUP_HEADER ResourceGroup
    ) {
	TIMER_INITIALIZATION_BLOCK NewTimer = {0};
	MCT_INTERNAL_DATA Data = {0};

#if LOAD_TEST
	KeBugCheckEx(MCT_LOAD_TEST_BUGCHECK, ResourceGroup->VendorId, ResourceGroup->DeviceId, Handle, 0);
#endif

	NewTimer.Header.TableVersion = 1;
	NewTimer.Header.TableSize = sizeof(NewTimer);
	NewTimer.Capabilities = TIMER_COUNTER_READABLE;
	// TIMER_ALWAYS_ON: the MCT runs off the 24 MHz crystal in every idle state.
	// If this HAL doesn't know the flag, register without it.
	NewTimer.Capabilities |= 0x8000;
	NewTimer.CounterBitWidth = 64;
	NewTimer.CounterFrequency = MCT_FREQUENCY;
	NewTimer.FunctionTable.Initialize = (PTIMER_INITIALIZE)MctInitialize;
	NewTimer.FunctionTable.QueryCounter = (PTIMER_QUERY_COUNTER)MctQueryCounter;
	NewTimer.InternalData = &Data;
	NewTimer.InternalDataSize = sizeof(Data);
	NewTimer.KnownType = TimerUnknown;
	NewTimer.MaxDivisor = 1;
	NewTimer.Identifier = 0;
	Data.Type = MctTimerCounter;
	NTSTATUS Status = HextRegisterTimer(Handle, ResourceGroup, &NewTimer);
	if (!NT_SUCCESS(Status)) {
		NewTimer.Capabilities &= ~0x8000;
		Status = HextRegisterTimer(Handle, ResourceGroup, &NewTimer);
		if (!NT_SUCCESS(Status)) return Status;
	}

	NewTimer.Interrupt.Gsi = MCT_G0_GSIV;
	NewTimer.Interrupt.Polarity = InterruptActiveHigh;
	NewTimer.Interrupt.Mode = LevelSensitive;
	NewTimer.Capabilities = TIMER_PSEUDO_PERIODIC_CAPABLE | TIMER_ONE_SHOT_CAPABLE | TIMER_GENERATES_LINE_BASED_INTERRUPTS;
	NewTimer.FunctionTable.QueryCounter = NULL;
	NewTimer.FunctionTable.ArmTimer = (PTIMER_CORE_TIMER)MctArmTimer;
	NewTimer.FunctionTable.AcknowledgeInterrupt = (PTIMER_ACKNOWLEDGE_INTERRUPT)MctAcknowledgeInterrupt;
	NewTimer.FunctionTable.Stop = (PTIMER_STOP)MctStop;
	NewTimer.Identifier = 1;
	Data.Type = MctTimerInterrupts;
	return HextRegisterTimer(Handle, ResourceGroup, &NewTimer);
}
