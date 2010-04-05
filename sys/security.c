/*
  Dokan : user-mode file system library for Windows

  Copyright (C) 2010 Hiroki Asakawa info@dokan-dev.net

  http://dokan-dev.net/en

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU Lesser General Public License as published by the Free
Software Foundation; either version 3 of the License, or (at your option) any
later version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU Lesser General Public License along
with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "dokan.h"

NTSTATUS
DokanDispatchQuerySecurity(
	__in PDEVICE_OBJECT DeviceObject,
	__in PIRP Irp
	)
{
	PIO_STACK_LOCATION	irpSp;
	NTSTATUS			status = STATUS_NOT_IMPLEMENTED;
	PFILE_OBJECT		fileObject;
	ULONG				info = 0;
	ULONG				bufferLength;
	SECURITY_DESCRIPTOR dummySecurityDesc;
	ULONG				descLength;
	PSECURITY_DESCRIPTOR securityDesc;
	PSECURITY_INFORMATION securityInfo;
	PDokanFCB			fcb;
	PDokanDCB			dcb;
	PDokanVCB			vcb;
	PDokanCCB			ccb;
	BOOLEAN				dummy = FALSE;
	ULONG				eventLength;
	PEVENT_CONTEXT		eventContext;

	__try {
		FsRtlEnterFileSystem();

		DDbgPrint("==> DokanQuerySecurity\n");

		irpSp = IoGetCurrentIrpStackLocation(Irp);
		fileObject = irpSp->FileObject;

		if (fileObject == NULL) {
			DDbgPrint("  fileObject == NULL\n");
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}

		vcb = DeviceObject->DeviceExtension;
		if (GetIdentifierType(vcb) != VCB) {
			DbgPrint("    DeviceExtension != VCB\n");
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}
		dcb = vcb->Dcb;

		DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));
		DDbgPrint("  FileName:%wZ\n", &fileObject->FileName);

		ccb = fileObject->FsContext2;
		if (ccb == NULL) {
			DDbgPrint("    ccb == NULL\n");
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}
		fcb = ccb->Fcb;

		bufferLength = irpSp->Parameters.QuerySecurity.Length;
		securityInfo = &irpSp->Parameters.QuerySecurity.SecurityInformation;

		switch (*securityInfo) {
		case DACL_SECURITY_INFORMATION:
			DDbgPrint("    DACL_SECURITY_INFORMATION\n");
			break;
		case GROUP_SECURITY_INFORMATION:
			DDbgPrint("    GROUP_SECURITY_INFORMATION\n");
			break;
		case OWNER_SECURITY_INFORMATION:
			DDbgPrint("    OWNER_SECURITY_INFORMATION\n");
			break;
		case SACL_SECURITY_INFORMATION:
			DDbgPrint("    SACL_SECURITY_INFORMATION\n");
			break;
		case LABEL_SECURITY_INFORMATION:
			DDbgPrint("    LABEL_SECURITY_INFORMATION\n");
			break;
		default:
			DDbgPrint("    Unknown Security information\n");
			break;
		}

		if (dummy) {
			RtlZeroMemory(&dummySecurityDesc, sizeof(SECURITY_DESCRIPTOR));
			status = RtlCreateSecurityDescriptor(&dummySecurityDesc, SECURITY_DESCRIPTOR_REVISION);
			if (!NT_SUCCESS(status)) {
				DDbgPrint("  RtlCreateSecurityDescriptor failed: 0x%x\n", status);
				__leave;
			}
			descLength = RtlLengthSecurityDescriptor(&dummySecurityDesc);

			if (bufferLength < descLength || Irp->UserBuffer == NULL) {
				status = STATUS_BUFFER_OVERFLOW;
				info = descLength;
				__leave;
			}
			securityDesc = &dummySecurityDesc;
			status = SeQuerySecurityDescriptorInfo(
						securityInfo,
						Irp->UserBuffer,
						&bufferLength,
						&securityDesc);
			if (!NT_SUCCESS(status)) {
				DDbgPrint("  SeQuerySecurityDescriptorInfo failed: 0x%x\n", status);
				__leave;
			}
			info = bufferLength;
			status = STATUS_SUCCESS;
			__leave;
		}

		eventLength = sizeof(EVENT_CONTEXT) + fcb->FileName.Length;
		eventContext = AllocateEventContext(dcb, Irp, eventLength, ccb);

		if (eventContext == NULL) {
			status = STATUS_INSUFFICIENT_RESOURCES;
			__leave;
		}

		if (Irp->UserBuffer != NULL && bufferLength > 0) {
			// make a MDL for UserBuffer that can be used later on another thread context	
			if (Irp->MdlAddress == NULL) {
				status = DokanAllocateMdl(Irp, bufferLength);
				if (!NT_SUCCESS(status)) {
					ExFreePool(eventContext);
					__leave;
				}
			}
		}

		eventContext->Context = ccb->UserContext;
		eventContext->Security.SecurityInformation = *securityInfo;
		eventContext->Security.BufferLength = bufferLength;
	
		eventContext->Security.FileNameLength = fcb->FileName.Length;
		RtlCopyMemory(eventContext->Security.FileName,
				fcb->FileName.Buffer, fcb->FileName.Length);

		status = DokanRegisterPendingIrp(DeviceObject, Irp, eventContext);

	} __finally {

		if (status != STATUS_PENDING) {
			Irp->IoStatus.Status = status;
			Irp->IoStatus.Information = info;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
			DokanPrintNTStatus(status);
		}

		DDbgPrint("<== DokanQuerySecurity\n");
		FsRtlExitFileSystem();
	}

	return status;
}


VOID
DokanCompleteQuerySecurity(
	__in PIRP_ENTRY		IrpEntry,
	__in PEVENT_INFORMATION EventInfo
	)
{
	PIRP		irp;
	PIO_STACK_LOCATION	irpSp;
	NTSTATUS	status;
	PVOID		buffer = NULL;
	ULONG		bufferLength;
	ULONG		info = 0;

	DDbgPrint("==> DokanCompleteQuerySecurity\n");

	irp   = IrpEntry->Irp;
	irpSp = IrpEntry->IrpSp;	

	if (irp->MdlAddress) {
		buffer = MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority);
	}

	bufferLength = irpSp->Parameters.QuerySecurity.Length;

	if (EventInfo->Status == STATUS_SUCCESS &&
		EventInfo->BufferLength <= bufferLength &&
		buffer != NULL) {
		RtlCopyMemory(buffer, EventInfo->Buffer, EventInfo->BufferLength);
		info = EventInfo->BufferLength;
		status = STATUS_SUCCESS;

	} else if (EventInfo->Status == STATUS_BUFFER_OVERFLOW ||
			(EventInfo->Status == STATUS_SUCCESS && bufferLength < EventInfo->BufferLength)) {
		info = EventInfo->BufferLength;
		status = STATUS_BUFFER_OVERFLOW;

	} else {
		info = 0;
		status = STATUS_INVALID_PARAMETER;
	}

	if (irp->MdlAddress != NULL) {
		MmUnlockPages(irp->MdlAddress);
		IoFreeMdl(irp->MdlAddress);
		irp->MdlAddress = NULL;
	}

	irp->IoStatus.Status = status;
	irp->IoStatus.Information = info;
	IoCompleteRequest(irp, IO_NO_INCREMENT);

	DokanPrintNTStatus(status);

	DDbgPrint("<== DokanCompleteQuerySecurity\n");
}


NTSTATUS
DokanDispatchSetSecurity(
	__in PDEVICE_OBJECT DeviceObject,
	__in PIRP Irp
	)
{
	PDokanVCB			vcb;
	PDokanDCB			dcb;
	PIO_STACK_LOCATION	irpSp;
	NTSTATUS			status = STATUS_NOT_IMPLEMENTED;
	PFILE_OBJECT		fileObject;
	ULONG				info = 0;

	__try {
		FsRtlEnterFileSystem();

		DDbgPrint("==> DokanSetSecurity\n");

		irpSp = IoGetCurrentIrpStackLocation(Irp);
		fileObject = irpSp->FileObject;
		
		if (fileObject == NULL) {
			DDbgPrint("  fileObject == NULL\n");
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}

		vcb = DeviceObject->DeviceExtension;
		if (GetIdentifierType(vcb) != VCB) {
			status = STATUS_INVALID_PARAMETER;
			__leave;
		}
		dcb = vcb->Dcb;

		DDbgPrint("  ProcessId %lu\n", IoGetRequestorProcessId(Irp));
		DDbgPrint("  FileName:%wZ\n", &fileObject->FileName);

		status = STATUS_SUCCESS;

	} __finally {

		if (status != STATUS_PENDING) {
			Irp->IoStatus.Status = status;
			Irp->IoStatus.Information = info;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
			DokanPrintNTStatus(status);
		}

		DDbgPrint("<== DokanSetSecurity\n");
		FsRtlExitFileSystem();
	}

	return status;
}
