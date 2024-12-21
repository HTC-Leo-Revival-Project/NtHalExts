#include "halext.h"
#include <stddef.h>
#include <armintr.h>
#define BIT(x) (1ul << x)

typedef volatile ULONG VULONG;
typedef volatile ULONGLONG VULONGLONG;

typedef struct _QCOM8250_ARM_LOCAL_MAILBOX {
	VULONG Value[4];
} QCOM8250_ARM_LOCAL_MAILBOX;

typedef struct _QCOM8250_ARM_LOCAL {
	VULONG
		ARM_CONTROL,
		CORE_TIMER_PRESCALER_SUBTRACT,
		CORE_TIMER_PRESCALER,
		CORE_IRQ_CONTROL,
		PMU_CONTROL_SET,
		PMU_CONTROL_CLR,
		reg_18,
		CORE_TIMER[2],
		PERI_IRQ_ROUTE[2],
		AXI_COUNT,
		AXI_IRQ,
		LOCAL_TIMER_CONTROL,
		LOCAL_TIMER_IRQ,
		reg_3C,
		TIMER_IRQ[4],
		MAILBOX_IRQ[4],
		IRQ_SOURCE[4],
		FIQ_SOURCE[4];
	QCOM8250_ARM_LOCAL_MAILBOX 
		MAILBOX_SET[4];
	union {
		QCOM8250_ARM_LOCAL_MAILBOX
			MAILBOX_READ[4],
			MAILBOX_CLR[4];
	};
} QCOM8250_ARM_LOCAL, *PQCOM8250_ARM_LOCAL;

typedef struct _BCM2708_INTERRUPT_CONTROLLER {
	VULONG
		BasicPending,
		Pending[2],
		FiqControl,
		Enable[2],
		EnableBasic,
		Disable[2],
		DisableBasic;
} BCM2708_INTERRUPT_CONTROLLER, *PBCM2708_INTERRUPT_CONTROLLER;


typedef struct _QCOM8250_INTERRUPT_DATA {
	PQCOM8250_ARM_LOCAL ArmPeriphVirt;
	PBCM2708_INTERRUPT_CONTROLLER Vc4InterruptVirt;
	ULONG ControllerIdentifier;
	BOOLEAN LinesDescribed;
	ULONG GsiBase;
	ULONG LocalPriorityForCore[4];
	ULONG LocalPriorityForLine[4];
	KSPIN_LOCK SpinLock;
} QCOM8250_INTERRUPT_DATA, *PQCOM8250_INTERRUPT_DATA;

#define __mcr(coproc, opcode1, value, crn, crm, opcode2) \
	_MoveToCoprocessor(value, coproc, opcode1, crn, crm, opcode2)

#define __mrc _MoveFromCoprocessor

#define SET_INTERRUPT_PROBLEM(a,b,status) do { \
	HalSetInterruptProblem(a, b, (status)); \
	return (status); \
} while (0)

enum {
	QCOM8250_PROCESSOR_COUNT = 4,
	QCOM8250_LOCAL_INTERRUPT_COUNT = 32,
	QCOM8250_GLOBAL_INTERRUPT_COUNT = 72,
	QCOM8250_INTERRUPT_CAUSE_SINGLE_COUNT = 32,
	QCOM8250_INVALID_PRIORITY = 0xFF,
};

enum {
	QCOM8250_ARM_LOCAL_PHYS = 0x40000000,
	BCM2708_VC4_INTERRUPT_PHYS = 0x3F00B200,
};

typedef enum {
#define INTERRUPT_DEFINE(key,val) VECTOR_##key = val,
#include "ints.inc"
#undef INTERRUPT_DEFINE
} INTERRUPT_VECTOR;

typedef enum {
#define INTERRUPT_DEFINE(key,val) INTERRUPT_##key = (1ul << (val)),
#include "ints.inc"
#undef INTERRUPT_DEFINE
} INTERRUPT_SOURCE;

typedef struct _QCOM8250_INTERRUPT_MAP {
	union {
		struct {
			UCHAR
				Timer[QCOM8250_PROCESSOR_COUNT],
				Mailbox[QCOM8250_PROCESSOR_COUNT],
				Unused0,
				PerfMonitor;
		};
		UCHAR LocalController[QCOM8250_LOCAL_INTERRUPT_COUNT];
	};
	UCHAR
		Controller0[32],
		Controller1[32],
		Basic[8];
} QCOM8250_INTERRUPT_MAP;

#define QCOM8250_INTERRUPT_START(Element) ((ULONG)offsetof(QCOM8250_INTERRUPT_MAP, Element))
#define QCOM8250_INTERRUPT_COUNT(Element) ((ULONG)sizeof((((QCOM8250_INTERRUPT_MAP*)NULL)->Element)))
#define QCOM8250_INTERRUPT_END(Element) (QCOM8250_INTERRUPT_START(Element) + QCOM8250_INTERRUPT_COUNT(Element))
#define QCOM8250_INTERRUPT_WITHIN(Interrupt, Element) ((Interrupt >= QCOM8250_INTERRUPT_START(Element)) && (Interrupt < QCOM8250_INTERRUPT_END(Element)))
#define QCOM8250_INTERRUPT_OFFSET(Interrupt, Element) ((Interrupt) - QCOM8250_INTERRUPT_START(Element))
#define QCOM8250_INTERRUPT_VC4_START(Element) (QCOM8250_INTERRUPT_START(Element) - QCOM8250_INTERRUPT_END(LocalController))

#define QCOM8250_INTERRUPT_ASSERT(Element, Start, End) _Static_assert(QCOM8250_INTERRUPT_START(Element) == Start && QCOM8250_INTERRUPT_END(Element) == End, "Bad " #Element );

_Static_assert(QCOM8250_GLOBAL_INTERRUPT_COUNT == (sizeof(QCOM8250_INTERRUPT_MAP) - QCOM8250_INTERRUPT_END(LocalController)), "bad interrupt map");
QCOM8250_INTERRUPT_ASSERT(Timer, 0, 4);
QCOM8250_INTERRUPT_ASSERT(Mailbox, 4, 8);
QCOM8250_INTERRUPT_ASSERT(Unused0, 8, 9);
QCOM8250_INTERRUPT_ASSERT(PerfMonitor, 9, 10);
QCOM8250_INTERRUPT_ASSERT(LocalController, 0, 32);
QCOM8250_INTERRUPT_ASSERT(Controller0, 32, 64);
QCOM8250_INTERRUPT_ASSERT(Controller1, 64, 96);
QCOM8250_INTERRUPT_ASSERT(Basic, 96, 104);

static UCHAR s_LocalPriorities[QCOM8250_PROCESSOR_COUNT][QCOM8250_LOCAL_INTERRUPT_COUNT];
static UCHAR s_GlobalPriorities[QCOM8250_GLOBAL_INTERRUPT_COUNT];

static ULONG s_Handle;
static PCSRT_RESOURCE_GROUP_HEADER s_ResourceGroup;

static ULONG BcmpGetCurrentProcessor(void) {
	return KeGetCurrentProcessorNumberEx(NULL);
}

static NTSTATUS BcmpEnsureMmioMapped(PQCOM8250_INTERRUPT_DATA Qsd8250) {
	if (Qsd8250->ArmPeriphVirt == NULL) {
		PHYSICAL_ADDRESS ArmPeriphPhys;
		ArmPeriphPhys.QuadPart = QCOM8250_ARM_LOCAL_PHYS;
		Qsd8250->ArmPeriphVirt = (PQCOM8250_ARM_LOCAL) HalMapIoSpace(ArmPeriphPhys, PAGE_SIZE, MmNonCached);
		if (Qsd8250->ArmPeriphVirt == NULL) {
			SET_INTERRUPT_PROBLEM(Qsd8250, InterruptProblemNullParameter, STATUS_INSUFFICIENT_RESOURCES);
		}
	}
	if (Qsd8250->Vc4InterruptVirt == NULL) {
		PHYSICAL_ADDRESS Vc4InterruptPhys;
		Vc4InterruptPhys.QuadPart = BCM2708_VC4_INTERRUPT_PHYS;
		Qsd8250->Vc4InterruptVirt = (PBCM2708_INTERRUPT_CONTROLLER) HalMapIoSpace(Vc4InterruptPhys, sizeof(*Qsd8250->Vc4InterruptVirt), MmNonCached);
		if (Qsd8250->Vc4InterruptVirt == NULL) {
			SET_INTERRUPT_PROBLEM(Qsd8250, InterruptProblemNullParameter, STATUS_INSUFFICIENT_RESOURCES);
		}
		Qsd8250->Vc4InterruptVirt->DisableBasic = 
			Qsd8250->Vc4InterruptVirt->Disable[0] = 
			Qsd8250->Vc4InterruptVirt->Disable[1] = 
				0xFFFFFFFF;
	}
	return STATUS_SUCCESS;
}

static NTSTATUS BcmpDescribeLinesImpl(PQCOM8250_INTERRUPT_DATA Qsd8250) {
	INTERRUPT_LINE_DESCRIPTION Description = {0};
	Description.Type = InterruptLineOutputPin;
	Description.MinLine = 1;
	Description.MaxLine = 2;
	Description.ControllerIdentifier = Qsd8250->ControllerIdentifier;
	Description.OutputUnitId = 0xFFFFFFFF;
	Description.GsiBase = 0xFFFFFFFF;
	NTSTATUS Status = HextRegisterInterruptLines(s_Handle, s_ResourceGroup, &Description);
	if (!NT_SUCCESS(Status)) return Status;
	ULONG GsiBase = Qsd8250->GsiBase;
	Description.OutputUnitId = 0;
	
	
	Description.Type = InterruptLineProcessorLocal;
	Description.MinLine = 0;
	Description.MaxLine = 4;
	Description.GsiBase = GsiBase;
	Status = HextRegisterInterruptLines(s_Handle, s_ResourceGroup, &Description);
	if (!NT_SUCCESS(Status)) return Status;
	
	
	Description.Type = InterruptLineSoftwareOnlyProcessorLocal;
	Description.MinLine = 4;
	Description.MaxLine = 8;
	Description.GsiBase = GsiBase + 4;
	Status = HextRegisterInterruptLines(s_Handle, s_ResourceGroup, &Description);
	if (!NT_SUCCESS(Status)) return Status;
	
	Description.Type = InterruptLineProcessorLocal;
	Description.MinLine = 8;
	Description.MaxLine = 32;
	Description.GsiBase = GsiBase + 8;
	Status = HextRegisterInterruptLines(s_Handle, s_ResourceGroup, &Description);
	if (!NT_SUCCESS(Status)) return Status;
	
	Description.Type = InterruptLineStandardPin;
	Description.MinLine = 32;
	Description.MaxLine = 104;
	Description.GsiBase = GsiBase + 32;
	Status = HextRegisterInterruptLines(s_Handle, s_ResourceGroup, &Description);
	if (!NT_SUCCESS(Status)) return Status;
	
	return Status;
}

static NTSTATUS BcmpDescribeLines(PQCOM8250_INTERRUPT_DATA Qsd8250) {
	if (Qsd8250->LinesDescribed) return STATUS_SUCCESS;
	// Already done at M2.
	if (!HextVersionIsM2()) {
		BcmpDescribeLinesImpl(Qsd8250);
	}
	Qsd8250->LinesDescribed = TRUE;
	return STATUS_SUCCESS;
}

static UCHAR BcmpGetPriorityForLine(ULONG ProcessorNumber, ULONG InterruptNumber) {
	if (ProcessorNumber < QCOM8250_PROCESSOR_COUNT && InterruptNumber < QCOM8250_LOCAL_INTERRUPT_COUNT)
		return s_LocalPriorities[ProcessorNumber][InterruptNumber];
	InterruptNumber -= QCOM8250_LOCAL_INTERRUPT_COUNT;
	if (InterruptNumber < QCOM8250_GLOBAL_INTERRUPT_COUNT)
		return s_GlobalPriorities[InterruptNumber];
	return QCOM8250_INVALID_PRIORITY;
}

static void BcmpToggleInterrupt(PQCOM8250_INTERRUPT_DATA Qsd8250, ULONG InterruptNumber, ULONG ProcessorNumber, BOOLEAN Enable) {
	if (QCOM8250_INTERRUPT_WITHIN(InterruptNumber, Timer)) {
		ULONG Offset = QCOM8250_INTERRUPT_OFFSET(InterruptNumber, Timer);
		ULONG Value = Qsd8250->ArmPeriphVirt->TIMER_IRQ[ProcessorNumber];
		// mask out irq+fiq enabled bits
		ULONG MaskField = (1 << Offset) | (1 << (Offset + 4));
		Value &= ~MaskField;
		// set IRQ if enabled
		if (Enable) Value |= (1 << Offset);
		// set the register
		Qsd8250->ArmPeriphVirt->TIMER_IRQ[ProcessorNumber] = Value;
		return;
	}
	
	if (QCOM8250_INTERRUPT_WITHIN(InterruptNumber, Mailbox)) {
		ULONG Offset = QCOM8250_INTERRUPT_OFFSET(InterruptNumber, Mailbox);
		ULONG Value = Qsd8250->ArmPeriphVirt->MAILBOX_IRQ[ProcessorNumber];
		// mask out irq + fiq enabled bits
		ULONG MaskField = (1 << Offset) | (1 << (Offset + 4));
		Value &= ~MaskField;
		// set IRQ if enabled
		if (Enable) Value |= (1 << Offset);
		// set the register
		Qsd8250->ArmPeriphVirt->MAILBOX_IRQ[ProcessorNumber] = Value;
		return;
	}
	
	if (QCOM8250_INTERRUPT_WITHIN(InterruptNumber, Unused0)) {
		return;
	}
	
	if (QCOM8250_INTERRUPT_WITHIN(InterruptNumber, PerfMonitor)) {
		if (!Enable) {
			// clear IRQ + FIQ enabled bits
			ULONG MaskField = (1 << ProcessorNumber) | (1 << (ProcessorNumber + 4));
			Qsd8250->ArmPeriphVirt->PMU_CONTROL_CLR = MaskField;
			return;
		}
		Qsd8250->ArmPeriphVirt->PMU_CONTROL_CLR = (1 << (ProcessorNumber + 4));
		Qsd8250->ArmPeriphVirt->PMU_CONTROL_SET = (1 << ProcessorNumber);
		return;
	}
	
	if (InterruptNumber < QCOM8250_INTERRUPT_COUNT(LocalController)) {
		return;
	}
	
	// Global controller VC4 MMIO side.
	if (QCOM8250_INTERRUPT_WITHIN(InterruptNumber, Controller0)) {
		ULONG Offset = QCOM8250_INTERRUPT_OFFSET(InterruptNumber, Controller0);
		if (!Enable) {
			Qsd8250->Vc4InterruptVirt->Disable[0] = (1 << Offset);
		} else {
			Qsd8250->Vc4InterruptVirt->Enable[0] = (1 << Offset);
		}
		return;
	}
	
	if (QCOM8250_INTERRUPT_WITHIN(InterruptNumber, Controller1)) {
		ULONG Offset = QCOM8250_INTERRUPT_OFFSET(InterruptNumber, Controller1);
		if (!Enable) {
			Qsd8250->Vc4InterruptVirt->Disable[1] = (1 << Offset);
		} else {
			Qsd8250->Vc4InterruptVirt->Enable[1] = (1 << Offset);
		}
		return;
	}
	
	if (QCOM8250_INTERRUPT_WITHIN(InterruptNumber, Basic)) {
		ULONG Offset = QCOM8250_INTERRUPT_OFFSET(InterruptNumber, Basic);
		if (!Enable) {
			Qsd8250->Vc4InterruptVirt->DisableBasic = (1 << Offset);
		} else {
			Qsd8250->Vc4InterruptVirt->EnableBasic = (1 << Offset);
		}
		return;
	}
}

static void BcmpToggleAllInterrupts(PQCOM8250_INTERRUPT_DATA Qsd8250, ULONG ProcessorNumber) {
	ULONG CorePriority = Qsd8250->LocalPriorityForCore[ProcessorNumber];
	ULONG PriorityForLine = Qsd8250->LocalPriorityForLine[ProcessorNumber];
	if (CorePriority <= PriorityForLine)
		CorePriority = PriorityForLine;
	// VC4 interrupts only go to core 0. If this is for another core, only disable local controller interrupts.
	ULONG InterruptCount = sizeof(QCOM8250_INTERRUPT_MAP);
	if (ProcessorNumber != 0) InterruptCount = QCOM8250_INTERRUPT_END(LocalController);
	for (ULONG InterruptNumber = 0; InterruptNumber < InterruptCount; InterruptNumber++) {
		UCHAR Priority = BcmpGetPriorityForLine(ProcessorNumber, InterruptNumber);
		if (Priority == QCOM8250_INVALID_PRIORITY) continue;
		BOOLEAN Enable = Priority > CorePriority;
		BcmpToggleInterrupt(Qsd8250, InterruptNumber, ProcessorNumber, Enable);
	}
}

static ULONG BcmpGetSingleRaisedVector(ULONG IrqSources) {
	return (QCOM8250_INTERRUPT_CAUSE_SINGLE_COUNT - 1) - _arm_clz(IrqSources);
}

static ULONG BcmpGetRaisedVector(PQCOM8250_INTERRUPT_DATA Qsd8250, ULONG ProcessorNumber) {
	ULONG ArmRaised = Qsd8250->ArmPeriphVirt->IRQ_SOURCE[ProcessorNumber];
	ULONG ArmRaisedSingle = BcmpGetSingleRaisedVector(Qsd8250->ArmPeriphVirt->IRQ_SOURCE[ProcessorNumber]);
	if (ArmRaisedSingle == VECTOR_VC4) {
		ULONG InterruptRaised = BcmpGetSingleRaisedVector(Qsd8250->Vc4InterruptVirt->Pending[0]);
		if (InterruptRaised != 0xFFFFFFFF) {
			return QCOM8250_INTERRUPT_START(Controller0) + InterruptRaised;
		}
		InterruptRaised = BcmpGetSingleRaisedVector(Qsd8250->Vc4InterruptVirt->Pending[1]);
		if (InterruptRaised != 0xFFFFFFFF) {
			return QCOM8250_INTERRUPT_START(Controller1) + InterruptRaised;
		}
		InterruptRaised = BcmpGetSingleRaisedVector(Qsd8250->Vc4InterruptVirt->BasicPending);
		if (InterruptRaised != 0xFFFFFFFF) {
			return QCOM8250_INTERRUPT_START(Basic) + InterruptRaised;
		}
		return BcmpGetSingleRaisedVector(ArmRaised & ~INTERRUPT_VC4);
	}
	return ArmRaisedSingle;
}

static NTSTATUS BcmpInitializeLocalUnit(
	__in PQCOM8250_INTERRUPT_DATA Qsd8250,
	__in ULONG ProcessorNumber,
	__in ULONG SpuriousVector,
	__in ULONG StubVector,
	__in ULONG LocalErrorVector,
	__out PULONG LocalId
) {
	NTSTATUS Status = BcmpEnsureMmioMapped(Qsd8250);
	if (!NT_SUCCESS(Status)) return Status;
	*LocalId = __mrc(15, 0, 0, 0, 5) & 0xF;
	return STATUS_SUCCESS;
}

static NTSTATUS BcmpInitializeIoUnit(__in PQCOM8250_INTERRUPT_DATA Qsd8250) {
	NTSTATUS Status = BcmpEnsureMmioMapped(Qsd8250);
	if (!NT_SUCCESS(Status)) return Status;
	Status = BcmpDescribeLines(Qsd8250);
	if (!NT_SUCCESS(Status)) {
		SET_INTERRUPT_PROBLEM(Qsd8250, InterruptProblemInvalidId, Status);
	}
	return STATUS_SUCCESS;
}

static void BcmpSetPriority(__in PQCOM8250_INTERRUPT_DATA Qsd8250, __in ULONG NewPriority) {
	ULONG ProcessorNumber = BcmpGetCurrentProcessor();
	// Acquire the spinlock if required.
	BOOLEAN SpinLockAcquired = FALSE;
	if (ProcessorNumber == 0) {
		KeAcquireSpinLockAtDpcLevel(&Qsd8250->SpinLock);
		SpinLockAcquired = TRUE;
	}
	// Set the priority in the interrupt data
	Qsd8250->LocalPriorityForCore[ProcessorNumber] = NewPriority;
	// Mask off all interrupts appropriate for the new priority.
	BcmpToggleAllInterrupts(Qsd8250, ProcessorNumber);
	// Release the spinlock if needed.
	if (SpinLockAcquired) KeReleaseSpinLockFromDpcLevel(&Qsd8250->SpinLock);
}

static INTERRUPT_RESULT BcmpAcceptAndGetSource(__in PQCOM8250_INTERRUPT_DATA Qsd8250, __out PLONG Line, __out PULONG OpaqueData) {
	*Line = 0;
	*OpaqueData = 0;
	ULONG ProcessorNumber = BcmpGetCurrentProcessor();
	// Get a single raised interrupt for this processor.
	ULONG InterruptRaised = BcmpGetRaisedVector(Qsd8250, ProcessorNumber);
	
	// Check for spurious interrupt.
	if (InterruptRaised == 0xFFFFFFFF) return InterruptBeginSpurious;
	
	// Mailbox interrupt.
	if (QCOM8250_INTERRUPT_WITHIN(InterruptRaised, Mailbox)) {
		ULONG Offset = QCOM8250_INTERRUPT_OFFSET(InterruptRaised, Mailbox);
		// Clear the mailbox.
		Qsd8250->ArmPeriphVirt->MAILBOX_CLR[ProcessorNumber].Value[Offset] = 0xFFFFFFFF;
	}
	
	// Get the current priority.
	ULONG OldPriority = Qsd8250->LocalPriorityForLine[ProcessorNumber];
	// Set line and opaque data.
	*Line = InterruptRaised;
	*OpaqueData = (OldPriority << 16) | InterruptRaised;
	// Set the priority to the correct one for this interrupt.
	BcmpSetPriority(Qsd8250, BcmpGetPriorityForLine(ProcessorNumber, InterruptRaised));
	return InterruptBeginLine;
}

static void BcmpWriteEndOfInterrupt(__in PQCOM8250_INTERRUPT_DATA Qsd8250, __in ULONG OpaqueToken) {
	// Get the old priority from the opaque token.
	ULONG OldPriority = OpaqueToken >> 16;
	// Set the priority to the old one.
	BcmpSetPriority(Qsd8250, OldPriority);
}

static NTSTATUS BcmpSetLineStateInternal(__in PQCOM8250_INTERRUPT_DATA Qsd8250, __in PINTERRUPT_LINE Line, __in PINTERRUPT_LINE_STATE LineState) {
	BOOLEAN Enabled = (LineState->Flags & INTERRUPT_LINE_ENABLED) != 0;
	//if (LineState->EmulateActiveBoth) return STATUS_NOT_SUPPORTED;
	UCHAR Priority = QCOM8250_INVALID_PRIORITY;
	if (Enabled) {
		// M2-era HAL has no LineState->Priority
		// Code there calculates the priority by other means
		if (HextVersionIsM2()) {
			Priority = 0xF0 - (LineState->Vector & 0xF0);
		} else {
			Priority = LineState->Priority;
		}
	}
	// Get the affinity mask to set.
	ULONG AffinityMask = 0;
	switch (LineState->ProcessorTarget.Target) {
	case InterruptTargetPhysical:
		AffinityMask = (1 << LineState->ProcessorTarget.PhysicalTarget);
		break;
	case InterruptTargetLogicalFlat:
		AffinityMask = LineState->ProcessorTarget.LogicalFlatTarget;
		break;
	case InterruptTargetAllIncludingSelf:
		// BUGBUG: rs1 hal does the same as SelfOnly for this case.
		AffinityMask = (1 << QCOM8250_PROCESSOR_COUNT) - 1;
		break;
	case InterruptTargetSelfOnly:
		AffinityMask = (1 << BcmpGetCurrentProcessor());
		break;
	default:
		SET_INTERRUPT_PROBLEM(Qsd8250, InterruptProblemInvalidType, STATUS_NOT_SUPPORTED);
	}
	
	// Acquire the spinlock if processor 0 is included in the affinity mask.
	BOOLEAN SpinLockAcquired = FALSE;
	if ((AffinityMask & BIT(0)) != 0) {
		KeAcquireSpinLockAtDpcLevel(&Qsd8250->SpinLock);
		SpinLockAcquired = TRUE;
	}
	
	ULONG Interrupt = Line->Line;
	for (ULONG Processor = 0; Processor < QCOM8250_PROCESSOR_COUNT; Processor++) {
		// Do nothing if this processor isn't included in the affinity mask.
		if ((AffinityMask & BIT(Processor)) == 0) continue;
		
		BOOLEAN EnableForThisCore = FALSE;
		if (Priority != QCOM8250_INVALID_PRIORITY) {
			ULONG CorePriority = Qsd8250->LocalPriorityForCore[Processor];
			ULONG PriorityForLine = Qsd8250->LocalPriorityForLine[Processor];
			if (CorePriority <= PriorityForLine)
				CorePriority = PriorityForLine;
			EnableForThisCore = (Priority > CorePriority);
		}
		
		// Set the priority.
		if (QCOM8250_INTERRUPT_WITHIN(Interrupt, LocalController)) {
			s_LocalPriorities[Processor][Interrupt] = Priority;
		} else {
			ULONG Vc4Interrupt = Interrupt - QCOM8250_INTERRUPT_COUNT(LocalController);
			if (Vc4Interrupt < QCOM8250_GLOBAL_INTERRUPT_COUNT) {
				s_GlobalPriorities[Vc4Interrupt] = Priority;
			}
		}
		// Enable or disable the interrupt at the interrupt controller.
		BcmpToggleInterrupt(Qsd8250, Interrupt, Processor, EnableForThisCore && Enabled);
	}
	
	// Release the spinlock if needed.
	if (SpinLockAcquired) KeReleaseSpinLockFromDpcLevel(&Qsd8250->SpinLock);
	return STATUS_SUCCESS;
}

static NTSTATUS BcmpSetLineState(__in PQCOM8250_INTERRUPT_DATA Qsd8250, __in PINTERRUPT_LINE Line, __in PINTERRUPT_LINE_STATE NewState) {
	// th1+(?) has an interrupt controller capability flag for calling SetLineState with interrupts disabled.
	// Older builds don't have this, so do this in here.
	BOOLEAN InterruptsEnabled = (_ReadStatusReg(0) & 0x80) == 0;
	if (InterruptsEnabled) _disable();
	NTSTATUS Status = BcmpSetLineStateInternal(Qsd8250, Line, NewState);
	if (InterruptsEnabled) _enable();
	return Status;
}

static NTSTATUS BcmpRequestInterrupt(__in PQCOM8250_INTERRUPT_DATA Qsd8250, __in PINTERRUPT_LINE Line, __in PINTERRUPT_TARGET Target) {
	if (
		Line->Line != VECTOR_MAILBOX0 &&
		Line->Line != VECTOR_MAILBOX1 &&
		Line->Line != VECTOR_MAILBOX2 &&
		Line->Line != VECTOR_MAILBOX3
	) {
		// Only the core mailbox interrupts supported here.
		SET_INTERRUPT_PROBLEM(Qsd8250, InterruptProblemInvalidType, STATUS_NOT_SUPPORTED);
	}
	ULONG ProcessorNumber = BcmpGetCurrentProcessor();
	ULONG TargetMask = 0;
	switch (Target->Target) {
		case InterruptTargetAllIncludingSelf:
			TargetMask = (1 << QCOM8250_PROCESSOR_COUNT) - 1;
			break;
		case InterruptTargetAllExcludingSelf:
			TargetMask = ((1 << QCOM8250_PROCESSOR_COUNT) - 1) ^ (1 << ProcessorNumber);
			break;
		case InterruptTargetSelfOnly:
			TargetMask = (1 << ProcessorNumber);
			break;
		case InterruptTargetPhysical:
			TargetMask = (1 << Target->PhysicalTarget);
			break;
		case InterruptTargetLogicalFlat:
			TargetMask = Target->LogicalFlatTarget;
			break;
		default:
			SET_INTERRUPT_PROBLEM(Qsd8250, InterruptProblemInvalidType, STATUS_NOT_SUPPORTED);
			break;
	}
	ULONG Mailbox = Line->Line - VECTOR_MAILBOX0;
	for (ULONG Processor = 0; Processor < QCOM8250_PROCESSOR_COUNT; Processor++) {
		if ((TargetMask & BIT(Processor)) == 0) continue;
		Qsd8250->ArmPeriphVirt->MAILBOX_SET[Processor].Value[Mailbox] = (1 << ProcessorNumber);
	}
	return STATUS_SUCCESS;
}

static NTSTATUS BcmpStartProcessor(__in PQCOM8250_INTERRUPT_DATA Qsd8250, __in ULONG LocalUnitId, __in PVOID StartupCodeVirtual, __in ULONG StartupCodePhysical) {
	if (LocalUnitId == 0 || LocalUnitId > QCOM8250_PROCESSOR_COUNT) {
		return STATUS_INVALID_PARAMETER_2;
	}
	Qsd8250->ArmPeriphVirt->MAILBOX_SET[LocalUnitId].Value[3] = LocalUnitId;
	return STATUS_SUCCESS;
}

static NTSTATUS BcmpConvertId(__in PQCOM8250_INTERRUPT_DATA Qsd8250, __inout PULONG PhysicalId, __inout PINTERRUPT_TARGET LogicalTarget, __in BOOLEAN ToLogical) {
	if (ToLogical) {
		LogicalTarget->Target = InterruptTargetLogicalFlat;
		LogicalTarget->LogicalFlatTarget = BIT(*PhysicalId);
		return STATUS_SUCCESS;
	}
	
	if (LogicalTarget->Target != InterruptTargetLogicalFlat) {
		SET_INTERRUPT_PROBLEM(Qsd8250, InterruptProblemInvalidType, STATUS_NOT_SUPPORTED);
	}
	
	ULONG PhysicalTarget = LogicalTarget->PhysicalTarget;
	if (PhysicalTarget == 0) {
		SET_INTERRUPT_PROBLEM(Qsd8250, InterruptProblemInvalidType, STATUS_NOT_SUPPORTED);
	}
	
	if ( ((PhysicalTarget - 1) & PhysicalTarget) != 0 ) {
		SET_INTERRUPT_PROBLEM(Qsd8250, InterruptProblemInvalidType, STATUS_NOT_SUPPORTED);
	}
	
	ULONG Index = 0;
	while (TRUE) {
		*PhysicalId = Index;
		if ((PhysicalTarget & 1) != 0) return STATUS_SUCCESS;
		Index++;
		PhysicalTarget >>= 1;
	}

}

NTSTATUS
AddResourceGroup (
    __in ULONG Handle,
    __in PCSRT_RESOURCE_GROUP_HEADER ResourceGroup
) {
	memset(s_LocalPriorities, QCOM8250_INVALID_PRIORITY, sizeof(s_LocalPriorities));
	memset(s_GlobalPriorities, QCOM8250_INVALID_PRIORITY, sizeof(s_GlobalPriorities));
	s_Handle = Handle;
	s_ResourceGroup = ResourceGroup;
	
	INTERRUPT_INITIALIZATION_BLOCK Interrupt = {0};
	QCOM8250_INTERRUPT_DATA Data = {0};
	
	KeInitializeSpinLock(&Data.SpinLock);
	
	Interrupt.Header.TableVersion = 1;
	Interrupt.Header.TableSize = sizeof(Interrupt);
	Interrupt.InternalDataSize = sizeof(Data);
	Interrupt.InternalData = &Data;
	// Most builds will return STATUS_NOT_SUPPORTED if KnownType != InterruptControllerGic
	Interrupt.KnownType = InterruptControllerGic;
	Interrupt.MaxPriority = 15;
	Interrupt.Capabilities =
		INTERRUPT_CONTROLLER_HAS_LOCAL_UNIT |
		INTERRUPT_CONTROLLER_HAS_PRIORITY_SCHEME |
		INTERRUPT_CONTROLLER_SUPPORTS_LOGICAL_FLAT |
		INTERRUPT_CONTROLLER_SHORTHAND_MASK;
	Interrupt.FunctionTable.InitializeLocalUnit = (PINTERRUPT_INITIALIZE_LOCAL_UNIT)BcmpInitializeLocalUnit;
	Interrupt.FunctionTable.InitializeIoUnit = (PINTERRUPT_INITIALIZE_IO_UNIT)BcmpInitializeIoUnit;
	Interrupt.FunctionTable.SetPriority = (PINTERRUPT_SET_PRIORITY)BcmpSetPriority;
	Interrupt.FunctionTable.AcceptAndGetSource = (PINTERRUPT_ACCEPT_AND_GET_SOURCE)BcmpAcceptAndGetSource;
	Interrupt.FunctionTable.EndOfInterrupt = (PINTERRUPT_END_OF_INTERRUPT)BcmpWriteEndOfInterrupt;
	Interrupt.FunctionTable.SetLineState = (PINTERRUPT_SET_LINE_STATE)BcmpSetLineState;
	Interrupt.FunctionTable.RequestInterrupt = (PINTERRUPT_REQUEST_INTERRUPT)BcmpRequestInterrupt;
	Interrupt.FunctionTable.StartProcessor = (PINTERRUPT_START_PROCESSOR)BcmpStartProcessor;
	Interrupt.FunctionTable.ConvertId = (PINTERRUPT_CONVERT_ID)BcmpConvertId;
	
	LARGE_INTEGER PhysAddr;
	PhysAddr.QuadPart = QCOM8250_ARM_LOCAL_PHYS;
	NTSTATUS Status = HalRegisterPermanentAddressUsage(PhysAddr, PAGE_SIZE);
	if (!NT_SUCCESS(Status)) return Status;
	PhysAddr.QuadPart = BCM2708_VC4_INTERRUPT_PHYS;
	Status = HalRegisterPermanentAddressUsage(PhysAddr, sizeof(BCM2708_INTERRUPT_CONTROLLER));
	if (!NT_SUCCESS(Status)) return Status;
	
	Status = HextRegisterInterruptController(Handle, ResourceGroup, &Interrupt);
	if (!NT_SUCCESS(Status)) {
		KeBugCheckEx(0x5C, 0x31415926, Status, 0, 0);
	}
	// In M2, we need to register lines at this point.
	if (HextVersionIsM2()) {
		Status = BcmpDescribeLinesImpl(&Data);
		if (!NT_SUCCESS(Status)) {
			KeBugCheckEx(0x5C, 0x31415926, Status, 1, 0);
		}
	}
	return Status;
}