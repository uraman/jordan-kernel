/*
 * Copyright (c) 2006-2010 Trusted Logic S.A.
 * All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <asm/div64.h>
#include <asm/system.h>
#include <linux/version.h>
#include <asm/cputype.h>
#include <linux/interrupt.h>
#include <linux/page-flags.h>
#include <linux/pagemap.h>
#include <linux/vmalloc.h>
#include <linux/jiffies.h>
#include <linux/freezer.h>

#include "scxlnx_defs.h"
#include "scxlnx_comm.h"
#include "scx_protocol.h"
#include "scxlnx_util.h"
#include "scxlnx_conn.h"


/*---------------------------------------------------------------------------
 * Internal Constants
 *---------------------------------------------------------------------------*/

/*
 * shared memories descriptor constants
 */
#define DESCRIPTOR_B_MASK           (1 << 2)
#define DESCRIPTOR_C_MASK           (1 << 3)
#define DESCRIPTOR_S_MASK           (1 << 10)

#define L1_COARSE_DESCRIPTOR_BASE         (0x00000001)
#define L1_COARSE_DESCRIPTOR_ADDR_MASK    (0xFFFFFC00)
#define L1_COARSE_DESCRIPTOR_V13_12_SHIFT (5)

#define L2_PAGE_DESCRIPTOR_BASE              (0x00000003)
#define L2_PAGE_DESCRIPTOR_AP_APX_READ       (0x220)
#define L2_PAGE_DESCRIPTOR_AP_APX_READ_WRITE (0x30)

#define L2_INIT_DESCRIPTOR_BASE           (0x00000003)
#define L2_INIT_DESCRIPTOR_V13_12_SHIFT   (4)

/*
 * Reject an attempt to share a strongly-Ordered or Device memory
 * Strongly-Ordered:  TEX=0b000, C=0, B=0
 * Shared Device:     TEX=0b000, C=0, B=1
 * Non-Shared Device: TEX=0b010, C=0, B=0
 */
#define L2_TEX_C_B_MASK \
	((1<<8) | (1<<7) | (1<<6) | (1<<3) | (1<<2))
#define L2_TEX_C_B_STRONGLY_ORDERED \
	((0<<8) | (0<<7) | (0<<6) | (0<<3) | (0<<2))
#define L2_TEX_C_B_SHARED_DEVICE \
	((0<<8) | (0<<7) | (0<<6) | (0<<3) | (1<<2))
#define L2_TEX_C_B_NON_SHARED_DEVICE \
	((0<<8) | (1<<7) | (0<<6) | (0<<3) | (0<<2))

#define CACHE_S(x)      ((x) & (1 << 24))
#define CACHE_DSIZE(x)  (((x) >> 12) & 4095)

#define TIME_IMMEDIATE ((u64) 0x0000000000000000ULL)
#define TIME_INFINITE  ((u64) 0xFFFFFFFFFFFFFFFFULL)

#define sigkill_pending() \
	(signal_pending(current) && \
	 sigismember(&current->pending.signal, SIGKILL))


/*---------------------------------------------------------------------------
 * atomic operation definitions
 *---------------------------------------------------------------------------*/

/*----------------------------------------------------------------------------
 * SMC related operations
 *----------------------------------------------------------------------------*/

/*
 * Atomically updates the nSyncSerial_N and sTime_N register
 * nSyncSerial_N and sTime_N modifications are thread safe
 */
void SCXLNXCommSetCurrentTime(struct SCXLNX_COMM *pComm)
{
	u32 nNewSyncSerial;
	struct timeval now;
	u64 sTime64;

	/*
	 * lock the structure while updating the L1 shared memory fields
	 */
	spin_lock(&pComm->lock);

	/* read nSyncSerial_N and change the TimeSlot bit field */
	nNewSyncSerial =
		SCXLNXCommReadReg32(&pComm->pBuffer->nSyncSerial_N) + 1;

	do_gettimeofday(&now);
	sTime64 = now.tv_sec;
	sTime64 = (sTime64 * 1000) + (now.tv_usec / 1000);

	/* Write the new sTime and nSyncSerial into shared memory */
	SCXLNXCommWriteReg64(&pComm->pBuffer->sTime_N[nNewSyncSerial &
		SCX_SYNC_SERIAL_TIMESLOT_N], sTime64);
	SCXLNXCommWriteReg32(&pComm->pBuffer->nSyncSerial_N,
		nNewSyncSerial);

	spin_unlock(&pComm->lock);
}

/*
 * Performs the specific read timeout operation
 * The difficulty here is to read atomically 2 u32
 * values from the L1 shared buffer.
 * This is guaranteed by reading before and after the operation
 * the timeslot given by the Secure World
 */
static inline void SCXLNXCommReadTimeout(struct SCXLNX_COMM *pComm, u64 *pTime)
{
	u32 nSyncSerial_S_initial = 0;
	u32 nSyncSerial_S_final = 1;
	u64 sTime;

	spin_lock(&pComm->lock);

	while (nSyncSerial_S_initial != nSyncSerial_S_final) {
		nSyncSerial_S_initial = SCXLNXCommReadReg32(
			&pComm->pBuffer->nSyncSerial_S);
		sTime = SCXLNXCommReadReg64(
			&pComm->pBuffer->sTimeout_S[nSyncSerial_S_initial&1]);

		nSyncSerial_S_final = SCXLNXCommReadReg32(
			&pComm->pBuffer->nSyncSerial_S);
	}

	spin_unlock(&pComm->lock);

	*pTime = sTime;
}


/*----------------------------------------------------------------------------
 * Shared memory related operations
 *----------------------------------------------------------------------------*/

struct SCXLNX_COARSE_PAGE_TABLE *SCXLNXAllocateCoarsePageTable(
	struct SCXLNX_COARSE_PAGE_TABLE_ALLOCATION_CONTEXT *pAllocationContext,
	u32 nType)
{
	struct SCXLNX_COARSE_PAGE_TABLE *pCoarsePageTable = NULL;

	spin_lock(&(pAllocationContext->lock));

	if (!(list_empty(&(pAllocationContext->sFreeCoarsePageTables)))) {
		/*
		 * The free list can provide us a coarse page table
		 * descriptor
		 */
		pCoarsePageTable = list_entry(
				pAllocationContext->sFreeCoarsePageTables.next,
				struct SCXLNX_COARSE_PAGE_TABLE, list);
		list_del(&(pCoarsePageTable->list));

		pCoarsePageTable->pParent->nReferenceCount++;
	} else {
		/* no array of coarse page tables, create a new one */
		struct SCXLNX_COARSE_PAGE_TABLE_ARRAY *pArray;
		void *pPage;
		int i;

		/* first allocate a new page descriptor */
		pArray = internal_kmalloc(sizeof(*pArray), GFP_KERNEL);
		if (pArray == NULL) {
			printk(KERN_ERR "SCXLNXAllocateCoarsePageTable(%p):"
					" failed to allocate a table array\n",
					pAllocationContext);
			goto exit;
		}

		pArray->nType = nType;
		INIT_LIST_HEAD(&(pArray->list));

		/* now allocate the actual page the page descriptor describes */
		pPage = (void *) internal_get_zeroed_page(GFP_KERNEL);
		if (pPage == NULL) {
			printk(KERN_ERR "SCXLNXAllocateCoarsePageTable(%p):"
					" failed allocate a page\n",
					pAllocationContext);
			internal_kfree(pArray);
			goto exit;
		}

		/* initialize the coarse page table descriptors */
		for (i = 0; i < 4; i++) {
			INIT_LIST_HEAD(&(pArray->sCoarsePageTables[i].list));
			pArray->sCoarsePageTables[i].pDescriptors =
				pPage + (i * SIZE_1KB);
			pArray->sCoarsePageTables[i].pParent = pArray;

			if (i == 0) {
				/*
				 * the first element is kept for the current
				 * coarse page table allocation
				 */
				pCoarsePageTable =
					&(pArray->sCoarsePageTables[i]);
				pArray->nReferenceCount++;
			} else {
				/*
				 * The other elements are added to the free list
				 */
				list_add(&(pArray->sCoarsePageTables[i].list),
					&(pAllocationContext->
						sFreeCoarsePageTables));
			}
		}

		list_add(&(pArray->list),
			&(pAllocationContext->sCoarsePageTableArrays));
	}

exit:
	spin_unlock(&(pAllocationContext->lock));

	return pCoarsePageTable;
}


void SCXLNXFreeCoarsePageTable(
	struct SCXLNX_COARSE_PAGE_TABLE_ALLOCATION_CONTEXT *pAllocationContext,
	struct SCXLNX_COARSE_PAGE_TABLE *pCoarsePageTable,
	int nForce)
{
	struct SCXLNX_COARSE_PAGE_TABLE_ARRAY *pArray;

	spin_lock(&(pAllocationContext->lock));

	pArray = pCoarsePageTable->pParent;

	(pArray->nReferenceCount)--;

	if (pArray->nReferenceCount == 0) {
		/*
		 * no coarse page table descriptor is used
		 * check if we should free the whole page
		 */

		if ((pArray->nType == SCXLNX_PAGE_DESCRIPTOR_TYPE_PREALLOCATED)
			&& (nForce == 0))
			/*
			 * This is a preallocated page,
			 * add the page back to the free list
			 */
			list_add(&(pCoarsePageTable->list),
				&(pAllocationContext->sFreeCoarsePageTables));
		else {
			/*
			 * None of the page's coarse page table descriptors
			 * are in use, free the whole page
			 */
			int i;

			/*
			 * remove the page's associated coarse page table
			 * descriptors from the free list
			 */
			for (i = 0; i < 4; i++)
				if (&(pArray->sCoarsePageTables[i]) !=
						pCoarsePageTable)
					list_del(&(pArray->
						sCoarsePageTables[i].list));

			/*
			 * Free the page.
			 * The address of the page is contained in the first
			 * element
			 */
			internal_free_page((unsigned long) pArray->
				sCoarsePageTables[0].pDescriptors);
			pArray->sCoarsePageTables[0].pDescriptors = NULL;

			/* remove the coarse page table from the array  */
			list_del(&(pArray->list));

			/* finaly free the array */
			internal_kfree(pArray);
		}
	} else {
		/*
		 * Some coarse page table descriptors are in use.
		 * Add the descriptor to the free list
		 */
		list_add(&(pCoarsePageTable->list),
			&(pAllocationContext->sFreeCoarsePageTables));
	}

	spin_unlock(&(pAllocationContext->lock));
}


void SCXLNXInitializeCoarsePageTableAllocator(
	struct SCXLNX_COARSE_PAGE_TABLE_ALLOCATION_CONTEXT *pAllocationContext)
{
	spin_lock_init(&(pAllocationContext->lock));
	INIT_LIST_HEAD(&(pAllocationContext->sCoarsePageTableArrays));
	INIT_LIST_HEAD(&(pAllocationContext->sFreeCoarsePageTables));
}

void SCXLNXReleaseCoarsePageTableAllocator(
	struct SCXLNX_COARSE_PAGE_TABLE_ALLOCATION_CONTEXT *pAllocationContext)
{
	spin_lock(&(pAllocationContext->lock));

	/* now clean up the list of page descriptors */
	while (!list_empty(&(pAllocationContext->sCoarsePageTableArrays))) {
		struct SCXLNX_COARSE_PAGE_TABLE_ARRAY *pPageDesc;

		pPageDesc = list_entry(
			pAllocationContext->sCoarsePageTableArrays.next,
			struct SCXLNX_COARSE_PAGE_TABLE_ARRAY, list);

		if (pPageDesc->sCoarsePageTables[0].pDescriptors != NULL)
			internal_free_page((unsigned long) pPageDesc->
				sCoarsePageTables[0].pDescriptors);

		list_del(&(pPageDesc->list));
		internal_kfree(pPageDesc);
	}

	spin_unlock(&(pAllocationContext->lock));
}

/*
 * Returns the L1 coarse page descriptor for
 * a coarse page table located at address pCoarsePageTableDescriptors
 */
u32 SCXLNXCommGetL1CoarseDescriptor(
	u32 pCoarsePageTableDescriptors[256])
{
	u32 nDescriptor = L1_COARSE_DESCRIPTOR_BASE;
	unsigned int info = read_cpuid(CPUID_CACHETYPE);

	nDescriptor |= (virt_to_phys((void *) pCoarsePageTableDescriptors)
		& L1_COARSE_DESCRIPTOR_ADDR_MASK);

	if (CACHE_S(info) && (CACHE_DSIZE(info) & (1 << 11))) {
		dprintk(KERN_DEBUG "SCXLNXCommGetL1CoarseDescriptor "
			"V31-12 added to descriptor\n");
		/* the 16k alignment restriction applies */
		nDescriptor |= (DESCRIPTOR_V13_12_GET(
			(u32)pCoarsePageTableDescriptors) <<
				L1_COARSE_DESCRIPTOR_V13_12_SHIFT);
	}

	return nDescriptor;
}


#define dprintk_desc(...)
/*
 * Returns the L2 descriptor for the specified user page.
 */
u32 SCXLNXCommGetL2DescriptorCommon(u32 nVirtAddr, struct mm_struct *mm)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *ptep;
	u32  *hwpte;
	u32   tex = 0;
	u32 nDescriptor = 0;

	dprintk_desc(KERN_INFO "VirtAddr = %x\n", nVirtAddr);
	pgd = pgd_offset(mm, nVirtAddr);
	dprintk_desc(KERN_INFO "pgd = %x, value=%x\n", (unsigned int) pgd,
		(unsigned int) *pgd);
	if (pgd_none(*pgd))
		goto error;
	pud = pud_offset(pgd, nVirtAddr);
	dprintk_desc(KERN_INFO "pud = %x, value=%x\n", (unsigned int) pud,
		(unsigned int) *pud);
	if (pud_none(*pud))
		goto error;
	pmd = pmd_offset(pud, nVirtAddr);
	dprintk_desc(KERN_INFO "pmd = %x, value=%x\n", (unsigned int) pmd,
		(unsigned int) *pmd);
	if (pmd_none(*pmd))
		goto error;

	if (PMD_TYPE_SECT&(*pmd)) {
		/* We have a section */
		dprintk_desc(KERN_INFO "Section descr=%x\n",
			(unsigned int)*pmd);
		if ((*pmd) & PMD_SECT_BUFFERABLE)
			nDescriptor |= DESCRIPTOR_B_MASK;
		if ((*pmd) & PMD_SECT_CACHEABLE)
			nDescriptor |= DESCRIPTOR_C_MASK;
		if ((*pmd) & PMD_SECT_S)
			nDescriptor |= DESCRIPTOR_S_MASK;
		tex = ((*pmd) >> 12) & 7;
	} else {
		/* We have a table */
		ptep = pte_offset_map(pmd, nVirtAddr);
		if (pte_present(*ptep)) {
			dprintk_desc(KERN_INFO "L2 descr=%x\n",
				(unsigned int) *ptep);
			if ((*ptep) & L_PTE_MT_BUFFERABLE)
				nDescriptor |= DESCRIPTOR_B_MASK;
			if ((*ptep) & L_PTE_MT_WRITETHROUGH)
				nDescriptor |= DESCRIPTOR_C_MASK;
			if ((*ptep) & L_PTE_MT_DEV_SHARED)
				nDescriptor |= DESCRIPTOR_S_MASK;

			/*
			 * Linux's pte doesn't keep track of TEX value.
			 * Have to jump to hwpte see include/asm/pgtable.h
			 */
			hwpte = (u32 *) (((u32) ptep) - 0x800);
			if (((*hwpte) & L2_DESCRIPTOR_ADDR_MASK) !=
					((*ptep) & L2_DESCRIPTOR_ADDR_MASK))
				goto error;
			dprintk_desc(KERN_INFO "hw descr=%x\n", *hwpte);
			tex = ((*hwpte) >> 6) & 7;
			pte_unmap(ptep);
		} else {
			pte_unmap(ptep);
			goto error;
		}
	}

	nDescriptor |= (tex << 6);

	return nDescriptor;

error:
	dprintk(KERN_ERR "Error occured in %s\n", __func__);
	return 0;
}


/*
 * Changes an L2 page descriptor back to a pointer to a physical page
 */
inline struct page *SCXLNXCommL2PageDescriptorToPage(u32 nL2PageDescriptor)
{
	return pte_page(nL2PageDescriptor & L2_DESCRIPTOR_ADDR_MASK);
}


/*
 * Returns the L1 descriptor for the 1KB-aligned coarse page table. The address
 * must be in the kernel address space.
 */
void SCXLNXCommGetL2PageDescriptor(u32 *pL2PageDescriptor,
	u32 nFlags, struct mm_struct *mm)
{
	unsigned long nPageVirtAddr;
	u32 nDescriptor;

	if (*pL2PageDescriptor == L2_DESCRIPTOR_FAULT)
		return;

	nPageVirtAddr = (unsigned long) page_address(
		(struct page *) (*pL2PageDescriptor));

	nDescriptor = L2_PAGE_DESCRIPTOR_BASE;

	nDescriptor |= (page_to_phys((struct page *) (*pL2PageDescriptor)) &
		L2_DESCRIPTOR_ADDR_MASK);

	if (!(nFlags & SCX_SHMEM_TYPE_WRITE))
		/* only read access */
		nDescriptor |= L2_PAGE_DESCRIPTOR_AP_APX_READ;
	else
		/* read and write access */
		nDescriptor |= L2_PAGE_DESCRIPTOR_AP_APX_READ_WRITE;

	nDescriptor |= SCXLNXCommGetL2DescriptorCommon(nPageVirtAddr, mm);

	*pL2PageDescriptor = nDescriptor;
}


/*
 * Unlocks the physical memory pages
 * and frees the coarse pages that need to
 */
void SCXLNXCommReleaseSharedMemory(
	struct SCXLNX_COARSE_PAGE_TABLE_ALLOCATION_CONTEXT *pAllocationContext,
	struct SCXLNX_SHMEM_DESC *pShmemDesc,
	u32 nFullCleanup)
{
	u32 nCoarsePageIndex;

	dprintk(KERN_INFO "SCXLNXCommReleaseSharedMemory(%p)\n",
			pShmemDesc);

#ifdef DEBUG_COARSE_TABLES
	printk(KERN_DEBUG "SCXLNXCommReleaseSharedMemory "
		"- numberOfCoarsePages=%d\n",
		pShmemDesc->nNumberOfCoarsePageTables);

	for (nCoarsePageIndex = 0;
	     nCoarsePageIndex < pShmemDesc->nNumberOfCoarsePageTables;
	     nCoarsePageIndex++) {
		u32 nIndex;

		printk(KERN_DEBUG "  Descriptor=%p address=%p index=%d\n",
			pShmemDesc->pCoarsePageTable[nCoarsePageIndex],
			pShmemDesc->pCoarsePageTable[nCoarsePageIndex]->
				pDescriptors,
			nCoarsePageIndex);
		if (pShmemDesc->pCoarsePageTable[nCoarsePageIndex] != NULL) {
			for (nIndex = 0;
			     nIndex < SCX_DESCRIPTOR_TABLE_CAPACITY;
			     nIndex += 8) {
				int i;
				printk(KERN_DEBUG "    ");
				for (i = nIndex; i < nIndex + 8; i++)
					printk(KERN_DEBUG "%p ",
						pShmemDesc->pCoarsePageTable[
							nCoarsePageIndex]->
								pDescriptors);
				printk(KERN_DEBUG "\n");
			}
		}
	}
	printk(KERN_DEBUG "SCXLNXCommReleaseSharedMemory() - done\n\n");
#endif

	/* Parse the coarse page descriptors */
	for (nCoarsePageIndex = 0;
	     nCoarsePageIndex < pShmemDesc->nNumberOfCoarsePageTables;
	     nCoarsePageIndex++) {
		u32 nPageIndex;
		u32 nFoundStart = 0;

		/* parse the page descriptors of the coarse page */
		for (nPageIndex = 0;
		     nPageIndex < SCX_DESCRIPTOR_TABLE_CAPACITY;
		     nPageIndex++) {
			u32 nL2PageDescriptor = (u32) (pShmemDesc->
				pCoarsePageTable[nCoarsePageIndex]->
					pDescriptors[nPageIndex]);

			if (nL2PageDescriptor != L2_DESCRIPTOR_FAULT) {
				struct page *page =
					SCXLNXCommL2PageDescriptorToPage(
						nL2PageDescriptor);

				if (!PageReserved(page))
					SetPageDirty(page);
				internal_page_cache_release(page);

				nFoundStart = 1;
			} else if (nFoundStart == 1) {
				break;
			}
		}

		/*
		 * Only free the coarse pages of descriptors not preallocated
		 */
		if ((pShmemDesc->nType == SCXLNX_SHMEM_TYPE_REGISTERED_SHMEM) ||
			(nFullCleanup != 0))
			SCXLNXFreeCoarsePageTable(pAllocationContext,
				pShmemDesc->pCoarsePageTable[nCoarsePageIndex],
				0);
	}

	pShmemDesc->nNumberOfCoarsePageTables = 0;
	dprintk(KERN_INFO "SCXLNXCommReleaseSharedMemory(%p) done\n",
			pShmemDesc);
}


/*
 * Make sure the coarse pages are allocated. If not allocated, do it Locks down
 * the physical memory pages
 * Verifies the memory attributes depending on nFlags
 */
int SCXLNXCommFillDescriptorTable(
	struct SCXLNX_COARSE_PAGE_TABLE_ALLOCATION_CONTEXT *pAllocationContext,
	struct SCXLNX_SHMEM_DESC *pShmemDesc,
	u32 nBufferVAddr,
	struct vm_area_struct **ppVmas,
	u32 pDescriptors[SCX_MAX_COARSE_PAGES],
	u32 *pBufferSize,
	u32 *pBufferStartOffset,
	bool bInUserSpace,
	u32 nFlags,
	u32 *pnDescriptorCount)
{
	u32 nCoarsePageIndex;
	u32 nNumberOfCoarsePages;
	u32 nPageCount;
	u32 nPageShift = 0;
	u32 nIndex;
	u32 nBufferSize = *pBufferSize;
	int nError;
	unsigned int info = read_cpuid(CPUID_CACHETYPE);

	dprintk(KERN_INFO "SCXLNXCommFillDescriptorTable"
		"(%p, nBufferVAddr=0x%08X, size=0x%08X, user=%01x "
		"flags = 0x%08x)\n",
		pShmemDesc,
		nBufferVAddr,
		nBufferSize,
		bInUserSpace,
		nFlags);

	/*
	 * Compute the number of pages
	 * Compute the number of coarse pages
	 * Compute the page offset
	 */
	nPageCount = ((nBufferVAddr & ~PAGE_MASK) +
		nBufferSize + ~PAGE_MASK) >> PAGE_SHIFT;

	/* check whether the 16k alignment restriction applies */
	if (CACHE_S(info) && (CACHE_DSIZE(info) & (1 << 11)))
		/*
		 * The 16k alignment restriction applies.
		 * Shift data to get them 16k aligned
		 */
		nPageShift = DESCRIPTOR_V13_12_GET(nBufferVAddr);
	nPageCount += nPageShift;


	/*
	 * Check the number of pages fit in the coarse pages
	 */
	if (nPageCount > (SCX_DESCRIPTOR_TABLE_CAPACITY *
			SCX_MAX_COARSE_PAGES)) {
		dprintk(KERN_ERR "SCXLNXCommFillDescriptorTable(%p): "
			"%u pages required to map shared memory!\n",
			pShmemDesc, nPageCount);
		nError = -ENOMEM;
		goto error;
	}

	/* coarse page describe 256 pages */
	nNumberOfCoarsePages = ((nPageCount +
		SCX_DESCRIPTOR_TABLE_CAPACITY_MASK) >>
			SCX_DESCRIPTOR_TABLE_CAPACITY_BIT_SHIFT);

	/*
	 * Compute the buffer offset
	 */
	*pBufferStartOffset = (nBufferVAddr & ~PAGE_MASK) |
		(nPageShift << PAGE_SHIFT);

	/* map each coarse page */
	for (nCoarsePageIndex = 0;
	     nCoarsePageIndex < nNumberOfCoarsePages;
	     nCoarsePageIndex++) {
		struct SCXLNX_COARSE_PAGE_TABLE *pCoarsePageTable;

		/* compute a virual address with appropriate offset */
		u32 nBufferOffsetVAddr = nBufferVAddr +
			(nCoarsePageIndex * SCX_MAX_COARSE_PAGE_MAPPED_SIZE);
		u32 nPagesToGet;

		/*
		 * Compute the number of pages left for this coarse page.
		 * Decrement nPageCount each time
		 */
		nPagesToGet = (nPageCount >>
			SCX_DESCRIPTOR_TABLE_CAPACITY_BIT_SHIFT) ?
				SCX_DESCRIPTOR_TABLE_CAPACITY : nPageCount;
		nPageCount -= nPagesToGet;

		/*
		 * Check if the coarse page has already been allocated
		 * If not, do it now
		 */
		if ((pShmemDesc->nType == SCXLNX_SHMEM_TYPE_REGISTERED_SHMEM)
			|| (pShmemDesc->nType ==
				SCXLNX_SHMEM_TYPE_PM_HIBERNATE)) {
			pCoarsePageTable = SCXLNXAllocateCoarsePageTable(
				pAllocationContext,
				SCXLNX_PAGE_DESCRIPTOR_TYPE_NORMAL);

			if (pCoarsePageTable == NULL) {
				printk(KERN_ERR
					"SCXLNXCommFillDescriptorTable(%p):"
					" SCXLNXConnAllocateCoarsePageTable "
					"failed for coarse page %d\n",
					pShmemDesc, nCoarsePageIndex);
				nError = -ENOMEM;
				goto error;
			}

			pShmemDesc->pCoarsePageTable[nCoarsePageIndex] =
				pCoarsePageTable;
		} else {
			pCoarsePageTable =
				pShmemDesc->pCoarsePageTable[nCoarsePageIndex];
		}

		/*
		 * The page is not necessarily filled with zeroes.
		 * Set the fault descriptors ( each descriptor is 4 bytes long)
		 */
		memset(pCoarsePageTable->pDescriptors, 0x00,
			SCX_DESCRIPTOR_TABLE_CAPACITY * sizeof(u32));

		if (bInUserSpace) {
			int nPages;

			/*
			 * TRICK: use pCoarsePageDescriptor->pDescriptors to
			 * hold the (struct page*) items before getting their
			 * physical address
			 */
			down_read(&(current->mm->mmap_sem));
			nPages = internal_get_user_pages(
				current,
				current->mm,
				nBufferOffsetVAddr,
				/*
				 * nPageShift is cleared after retrieving first
				 * coarse page
				 */
				(nPagesToGet - nPageShift),
				(nFlags & SCX_SHMEM_TYPE_WRITE) ? 1 : 0,
				0,
				(struct page **) (pCoarsePageTable->pDescriptors
					+ nPageShift),
				ppVmas);
			up_read(&(current->mm->mmap_sem));

			if ((nPages <= 0) ||
				(nPages != (nPagesToGet - nPageShift))) {
				dprintk(KERN_ERR"SCXLNXCommFillDescriptorTable:"
					" get_user_pages got %d pages while "
					"trying to get %d pages!\n",
					nPages, nPagesToGet - nPageShift);
				nError = -EFAULT;
				goto error;
			}

			for (nIndex = nPageShift;
			     nIndex < nPageShift + nPages;
			     nIndex++) {
				/* Get the actual L2 descriptors */
				SCXLNXCommGetL2PageDescriptor(
					&pCoarsePageTable->pDescriptors[nIndex],
					nFlags,
					current->mm);
				/*
				 * Reject Strongly-Ordered or Device Memory
				 */
#define IS_STRONGLY_ORDERED_OR_DEVICE_MEM(x) \
	((((x) & L2_TEX_C_B_MASK) == L2_TEX_C_B_STRONGLY_ORDERED) || \
	 (((x) & L2_TEX_C_B_MASK) == L2_TEX_C_B_SHARED_DEVICE) || \
	 (((x) & L2_TEX_C_B_MASK) == L2_TEX_C_B_NON_SHARED_DEVICE))

				if (IS_STRONGLY_ORDERED_OR_DEVICE_MEM(
					pCoarsePageTable->
						pDescriptors[nIndex])) {
					dprintk(KERN_ERR
						"SCXLNXCommFillDescriptorTable:"
						" descriptor 0x%08X use "
						"strongly-ordered or device "
						"memory. Rejecting!\n",
						pCoarsePageTable->
							pDescriptors[nIndex]);
					nError = -EFAULT;
					goto error;
				}
			}
		} else {
			/* Kernel-space memory */
			for (nIndex = nPageShift;
			     nIndex < nPagesToGet;
			     nIndex++) {
				unsigned long addr =
					(unsigned long) (nBufferOffsetVAddr +
						((nIndex - nPageShift) *
							PAGE_SIZE));
				pCoarsePageTable->pDescriptors[nIndex] =
					(u32) vmalloc_to_page((void *)addr);
				get_page((struct page *) pCoarsePageTable->
					pDescriptors[nIndex]);

				/* change coarse page "page address" */
				SCXLNXCommGetL2PageDescriptor(
					&pCoarsePageTable->pDescriptors[nIndex],
					nFlags,
					&init_mm);
			}
		}

#ifdef CONFIG_TF_MSHIELD
		/*
		 * Flush the coarse page table to synchronise with
		 * secure side
		 */
		flush_cache_all();
		outer_flush_range(
			__pa(pCoarsePageTable->pDescriptors),
			__pa(pCoarsePageTable->pDescriptors) +
			SCX_DESCRIPTOR_TABLE_CAPACITY * sizeof(u32));
		wmb();
#endif

		/* Update the coarse page table address */
		pDescriptors[nCoarsePageIndex] =
			SCXLNXCommGetL1CoarseDescriptor(
				pCoarsePageTable->pDescriptors);

		/*
		 * The next coarse page has no page shift, reset the
		 * nPageShift
		 */
		nPageShift = 0;
	}

	*pnDescriptorCount = nNumberOfCoarsePages;
	pShmemDesc->nNumberOfCoarsePageTables = nNumberOfCoarsePages;

#ifdef DEBUG_COARSE_TABLES
	printk(KERN_DEBUG "nSCXLNXCommFillDescriptorTable - size=0x%08X "
		"numberOfCoarsePages=%d\n", *pBufferSize,
		pShmemDesc->nNumberOfCoarsePageTables);
	for (nCoarsePageIndex = 0;
	     nCoarsePageIndex < pShmemDesc->nNumberOfCoarsePageTables;
	     nCoarsePageIndex++) {
		u32 nIndex;
		struct SCXLNX_COARSE_PAGE_TABLE *pCorsePageTable =
			pShmemDesc->pCoarsePageTable[nCoarsePageIndex];

		printk(KERN_DEBUG "  Descriptor=%p address=%p index=%d\n",
			pCorsePageTable,
			pCorsePageTable->pDescriptors,
			nCoarsePageIndex);
		for (nIndex = 0;
		     nIndex < SCX_DESCRIPTOR_TABLE_CAPACITY;
		     nIndex += 8) {
			int i;
			printk(KERN_DEBUG "    ");
			for (i = nIndex; i < nIndex + 8; i++)
				printk(KERN_DEBUG "0x%08X ",
					pCorsePageTable->pDescriptors[i]);
			printk(KERN_DEBUG "\n");
		}
	}
	printk(KERN_DEBUG "nSCXLNXCommFillDescriptorTable() - done\n\n");
#endif

	return 0;

error:
	SCXLNXCommReleaseSharedMemory(
			pAllocationContext,
			pShmemDesc,
			0);

	return nError;
}


/*----------------------------------------------------------------------------
 * Standard communication operations
 *----------------------------------------------------------------------------*/

u8 *SCXLNXCommGetDescription(struct SCXLNX_COMM *pComm)
{
	if (test_bit(SCXLNX_COMM_FLAG_L1_SHARED_ALLOCATED, &(pComm->nFlags)))
		return pComm->pBuffer->sVersionDescription;

	return NULL;
}

/*
 * Returns a non-zero value if the specified S-timeout has expired, zero
 * otherwise.
 *
 * The placeholder referenced to by pnRelativeTimeoutJiffies gives the relative
 * timeout from now in jiffies. It is set to zero if the S-timeout has expired,
 * or to MAX_SCHEDULE_TIMEOUT if the S-timeout is infinite.
 */
static int SCXLNXCommTestSTimeout(
		u64 sTimeout,
		signed long *pnRelativeTimeoutJiffies)
{
	struct timeval now;
	u64 sTime64;

	*pnRelativeTimeoutJiffies = 0;

	/* immediate timeout */
	if (sTimeout == TIME_IMMEDIATE)
		return 1;

	/* infinite timeout */
	if (sTimeout == TIME_INFINITE) {
		dprintk(KERN_DEBUG "SCXLNXCommTestSTimeout: "
			"timeout is infinite\n");
		*pnRelativeTimeoutJiffies = MAX_SCHEDULE_TIMEOUT;
		return 0;
	}

	do_gettimeofday(&now);
	sTime64 = now.tv_sec;
	/* will not overflow as operations are done on 64bit values */
	sTime64 = (sTime64 * 1000) + (now.tv_usec / 1000);

	/* timeout expired */
	if (sTime64 >= sTimeout) {
		dprintk(KERN_DEBUG "SCXLNXCommTestSTimeout: timeout expired\n");
		return 1;
	}

	/*
	 * finite timeout, compute pnRelativeTimeoutJiffies
	 */
	/* will not overflow as sTime64 < sTimeout */
	sTimeout -= sTime64;

	/* guarantee *pnRelativeTimeoutJiffies is a valid timeout */
	if ((sTimeout >> 32) != 0)
		*pnRelativeTimeoutJiffies = MAX_JIFFY_OFFSET;
	else
		*pnRelativeTimeoutJiffies =
			msecs_to_jiffies((unsigned int) sTimeout);

	dprintk(KERN_DEBUG "SCXLNXCommTestSTimeout: timeout is 0x%lx\n",
		*pnRelativeTimeoutJiffies);
	return 0;
}

/* Forward declaration */
static int SCXLNXCommSendMessage(
	struct SCXLNX_COMM *pComm,
	union SCX_COMMAND_MESSAGE *pMessage,
	struct SCXLNX_CONNECTION *pConn,
	int bKillable);


/*
 * Sends the specified message through the specified communication channel.
 *
 * This function sends the message and returns immediately
 *
 * Returns zero upon successful completion, or an appropriate error code upon
 * failure.
 */
static int SCXLNXCommSendMessage(struct SCXLNX_COMM *pComm,
	union SCX_COMMAND_MESSAGE *pMessage,
	struct SCXLNX_CONNECTION *pConn,
	int bKillable)
{
	bool bMessageCopied = false;
	u64 sTimeout;
	signed long nRelativeTimeoutJiffies;
	u32 nFirstFreeCommand;
	u32 nFirstCommand;
	u32 nFirstAnswer;
	u32 nFirstFreeAnswer;

	struct SCXLNX_ANSWER_STRUCT *pAnswerStructure;

	DEFINE_WAIT(wait);

	dprintk(KERN_INFO "SCXLNXCommSendMessage(%p)\n", pMessage);

	/*
	 * Read all answers from the answer queue
	 */
copy_answers:
	if (test_bit(SCXLNX_COMM_FLAG_L1_SHARED_ALLOCATED, &(pComm->nFlags))) {
		spin_lock(&pComm->lock);
		nFirstFreeAnswer = SCXLNXCommReadReg32(
			&pComm->pBuffer->nFirstFreeAnswer);
		nFirstAnswer = SCXLNXCommReadReg32(
			&pComm->pBuffer->nFirstAnswer);

		while (nFirstAnswer != nFirstFreeAnswer) {
			/* answer queue not empty */
			union SCX_ANSWER_MESSAGE sComAnswer;
			struct SCX_ANSWER_HEADER  sHeader;

			/*
			 * the size of the command in words of 32bit, not in
			 * bytes
			 */
			u32 nCommandSize;
			u32 i;
			u32 *pTemp = (uint32_t *) &sHeader;

			dprintk(KERN_INFO "SCXLNXCommSendMessage(%p): "
				"Read answers from L1\n", pMessage);

			/* Read the answer header */
			for (i = 0;
			     i < sizeof(struct SCX_ANSWER_HEADER)/sizeof(u32);
			       i++)
				pTemp[i] = pComm->pBuffer->sAnswerQueue[
					(nFirstAnswer + i) %
						SCX_S_ANSWER_QUEUE_CAPACITY];

			/* Read the answer from the L1_Buffer*/
			nCommandSize = sHeader.nMessageSize +
				sizeof(struct SCX_ANSWER_HEADER)/sizeof(u32);
			pTemp = (uint32_t *) &sComAnswer;
			for (i = 0; i < nCommandSize; i++)
				pTemp[i] = pComm->pBuffer->sAnswerQueue[
					(nFirstAnswer + i) %
						SCX_S_ANSWER_QUEUE_CAPACITY];

			pAnswerStructure = (struct SCXLNX_ANSWER_STRUCT *)
				sComAnswer.sHeader.nOperationID;

			SCXLNXDumpAnswer(&sComAnswer);

			memcpy(pAnswerStructure->pAnswer, &sComAnswer,
				nCommandSize * sizeof(u32));
			pAnswerStructure->bAnswerCopied = true;

			nFirstAnswer += nCommandSize;
			SCXLNXCommWriteReg32(&pComm->pBuffer->nFirstAnswer,
				nFirstAnswer);
		}
		spin_unlock(&(pComm->lock));
	}

	if ((test_bit(SCXLNX_COMM_FLAG_L1_SHARED_ALLOCATED, &(pComm->nFlags)))
			&& (pMessage != NULL)) {
		/*
		 * Write the message in the message queue.
		 */

		if (!bMessageCopied) {
			u32  nCommandSize;
			u32  nQueueWordsCount;
			u32  i;

			dprintk(KERN_INFO "SCXLNXCommSendMessage(%p): "
				"Write Message in the queue\n", pMessage);

			spin_lock(&pComm->lock);

			SCXLNXDumpMessage(pMessage);

			nFirstCommand = SCXLNXCommReadReg32(
				&pComm->pBuffer->nFirstCommand);
			nFirstFreeCommand = SCXLNXCommReadReg32(
				&pComm->pBuffer->nFirstFreeCommand);

			nQueueWordsCount = nFirstFreeCommand - nFirstCommand;
			nCommandSize     = pMessage->sHeader.nMessageSize +
				sizeof(struct SCX_COMMAND_HEADER)/sizeof(u32);
			if ((nQueueWordsCount + nCommandSize) <
					SCX_N_MESSAGE_QUEUE_CAPACITY) {
				/*
				 * Command queue is not full.  If the Command
				 * queue is full, the command will be copied at
				 * another iteration of the current function.
				 */
				for (i = 0; i < nCommandSize; i++)
					pComm->pBuffer->sCommandQueue[
						(nFirstFreeCommand + i) %
						SCX_N_MESSAGE_QUEUE_CAPACITY] =
						((uint32_t *) pMessage)[i];

				bMessageCopied = true;
				nFirstFreeCommand += nCommandSize;

				SCXLNXCommWriteReg32(
					&pComm->pBuffer->nFirstFreeCommand,
					nFirstFreeCommand);
			}
			spin_unlock(&pComm->lock);
		}
	}

	/*
	 * Notify all waiting threads
	 */
	wake_up(&(pComm->waitQueue));

#ifdef CONFIG_TF_MSHIELD
	if (unlikely(freezing(current))) {
		printk(KERN_INFO "SMC: Entering refrigerator\n");
		refrigerator();
		printk(KERN_INFO "SMC: Left refrigerator\n");
	}
#endif

#ifndef CONFIG_PREEMPT
	if (need_resched())
		schedule();
#endif

	/*
	 * Handle RPC (if any)
	 */
	if (SCXLNXCommExecuteRPCCommand(pComm) == RPC_NON_YIELD)
		goto schedule_secure_world;

	/*
	 * Join wait queue
	 */
	dprintk(KERN_INFO "SCXLNXCommSendMessage(%p): "
		"Prepare to wait\n", pMessage);
	prepare_to_wait(&pComm->waitQueue, &wait,
			bKillable ? TASK_INTERRUPTIBLE : TASK_UNINTERRUPTIBLE);

	/*
	 * Check if our answer is available
	 */
#ifdef CONFIG_TF_MSHIELD
	/* JCO: not sure this is going to work in the TZ version */
	if ((pMessage == NULL) &&
		 (test_bit(SCXLNX_COMM_FLAG_L1_SHARED_ALLOCATED,
			&(pComm->nFlags)))) {
		/* Secure world finished booting */
		finish_wait(&pComm->waitQueue, &wait);
		return 0;
	}
#endif
	if (pMessage != NULL) {
		pAnswerStructure = (struct SCXLNX_ANSWER_STRUCT *)
			pMessage->sHeader.nOperationID;

		if (pAnswerStructure->bAnswerCopied) {
			dprintk(KERN_INFO "SCXLNXCommSendMessage(thread=%u): "
				"Received answer\n", current->pid);
			finish_wait(&pComm->waitQueue, &wait);
			return 0;
		}
	}

	/*
	 * Check if a signal is pending
	 */
	if (bKillable && (sigkill_pending())) {
		dprintk(KERN_ERR "SCXLNXCommSendMessage(thread=%u): "
			"Failure (error %d)\n", current->pid, -EINTR);
		finish_wait(&pComm->waitQueue, &wait);
		return -EINTR;
	}

	/*
	 * Check if secure world is schedulable. It is schedulable if at
	 * least one of the following conditions holds:
	 * + it is still initializing (SCXLNX_COMM_FLAG_L1_SHARED_ALLOCATED
	 *   is not set);
	 * + there is a command in the queue;
	 * + the secure world timeout is zero.
	 */
	if (test_bit(SCXLNX_COMM_FLAG_L1_SHARED_ALLOCATED, &(pComm->nFlags))) {
		spin_lock(&pComm->lock);
		nFirstCommand = SCXLNXCommReadReg32(
			&pComm->pBuffer->nFirstCommand);
		nFirstFreeCommand = SCXLNXCommReadReg32(
			&pComm->pBuffer->nFirstFreeCommand);
		spin_unlock(&pComm->lock);
		SCXLNXCommReadTimeout(pComm, &sTimeout);
		if ((nFirstFreeCommand == nFirstCommand) &&
			 (SCXLNXCommTestSTimeout(sTimeout,
			&nRelativeTimeoutJiffies) == 0))
			/*
			 * If command queue is empty and if timeout has not
			 * expired secure world is not schedulable
			 */
			goto wait;
	}

	finish_wait(&pComm->waitQueue, &wait);

	/*
	 * Yield to the Secure World
	 */
schedule_secure_world:
	{
		int ret = SCXLNXCommYield(pComm);
		if (ret != 0)
			return ret;
	}
	goto copy_answers;

wait:
	if (bKillable && (sigkill_pending())) {
		dprintk(KERN_ERR "SCXLNXCommSendMessage(thread=%u): "
			"Failure (error %d)\n", current->pid, -EINTR);
		finish_wait(&pComm->waitQueue, &wait);
		return -EINTR;
	}

	if (nRelativeTimeoutJiffies == MAX_SCHEDULE_TIMEOUT)
		dprintk(KERN_INFO "SCXLNXCommSendMessage: "
			"prepare to sleep infinitely\n");
	else
		dprintk(KERN_INFO "SCXLNXCommSendMessage: "
			"prepare to sleep 0x%lx jiffies\n",
			nRelativeTimeoutJiffies);

	/* go to sleep */
	schedule_timeout(nRelativeTimeoutJiffies);

	dprintk(KERN_INFO "SCXLNXCommSendMessage: "
		"N_SM_EVENT signaled or timeout expired\n");
	finish_wait(&pComm->waitQueue, &wait);

	goto copy_answers;
}

/*
 * Sends the specified message through the specified communication channel.
 *
 * This function sends the message and waits for the corresponding answer
 * It may return if a signal needs to be delivered.
 *
 * If pConn is not NULL, before sending the message, this function checks that
 * it is still valid by calling the function SCXLNXConnCheckMessageValidity
 *
 * Returns zero upon successful completion, or an appropriate error code upon
 * failure.
 */
int SCXLNXCommSendReceive(struct SCXLNX_COMM *pComm,
	  union SCX_COMMAND_MESSAGE *pMessage,
	  union SCX_ANSWER_MESSAGE *pAnswer,
	  struct SCXLNX_CONNECTION *pConn,
	  bool bKillable)
{
	int nError;
	struct SCXLNX_ANSWER_STRUCT sAnswerStructure;

	sAnswerStructure.pAnswer = pAnswer;
	sAnswerStructure.bAnswerCopied = false;

	if (pMessage != NULL)
		pMessage->sHeader.nOperationID = (u32) &sAnswerStructure;

	dprintk(KERN_INFO "SCXLNXSMCommSendReceive: "
		"SCXLNXCommSendMessage\n");

#ifdef CONFIG_TF_MSHIELD
	if (!test_bit(SCXLNX_COMM_FLAG_PA_AVAILABLE, &pComm->nFlags)) {
		dprintk(KERN_ERR "SCXLNXCommSendReceive(%p): "
			"Secure world not started\n", pComm);

		return -EFAULT;
	}
#endif

	if (test_bit(SCXLNX_COMM_FLAG_TERMINATING, &(pComm->nFlags)) != 0) {
		dprintk(KERN_DEBUG "SCXLNXSMCommSendReceive: "
			"Flag Terminating is set\n");
		return 0;
	}

	if (pConn != NULL && !SCXLNXConnCheckMessageValidity(pConn,
			pMessage)) {
		/* We must not send the message after all... */
		nError = -ENOTTY;
		return nError;
	}


	/*
	 * Send the command
	 */
	nError = SCXLNXCommSendMessage(pComm, pMessage, pConn, bKillable);

	if (nError  == -EINTR) {
		struct SCXLNX_ANSWER_STRUCT sAnswerStructDestroyDeviceContext;

		/* means bKillable is true */
		dprintk(KERN_ERR
			"SCXLNXSMCommSendReceive: "
			"SCXLNXCommSendMessage failed (error %d) !\n", nError);

		/*
		 * Destroy device context
		 */
		sAnswerStructDestroyDeviceContext.pAnswer =  pAnswer;
		sAnswerStructDestroyDeviceContext.bAnswerCopied = false;

		pMessage->sHeader.nMessageType =
			SCX_MESSAGE_TYPE_DESTROY_DEVICE_CONTEXT;
		pMessage->sHeader.nMessageSize =
			(sizeof(struct SCX_COMMAND_DESTROY_DEVICE_CONTEXT) -
				sizeof(struct SCX_COMMAND_HEADER))/sizeof(u32);
		pMessage->sHeader.nOperationID =
			(u32) &sAnswerStructDestroyDeviceContext;
		pMessage->sDestroyDeviceContextMessage.hDeviceContext =
			pConn->hDeviceContext;
		goto destroy_context;
	}

	if (!bKillable && sigkill_pending()) {
		if ((pMessage->sHeader.nMessageType ==
			SCX_MESSAGE_TYPE_CREATE_DEVICE_CONTEXT) &&
			(pAnswer->sCreateDeviceContextAnswer.nErrorCode ==
				S_SUCCESS)) {
			dprintk(KERN_INFO "SCXLNXSMCommSendReceive: "
				"sending DESTROY_DEVICE_CONTEXT\n");
			sAnswerStructure.pAnswer =  pAnswer;
			sAnswerStructure.bAnswerCopied = false;

			pMessage->sHeader.nMessageType =
				SCX_MESSAGE_TYPE_DESTROY_DEVICE_CONTEXT;
			pMessage->sHeader.nMessageSize =
				(sizeof(struct
					SCX_COMMAND_DESTROY_DEVICE_CONTEXT) -
				 sizeof(struct SCX_COMMAND_HEADER))/sizeof(u32);
			pMessage->sHeader.nOperationID =
				(u32) &sAnswerStructure;
			pMessage->sDestroyDeviceContextMessage.hDeviceContext =
				pAnswer->sCreateDeviceContextAnswer.
					hDeviceContext;
			goto destroy_context;
		}
	}

	dprintk(KERN_INFO "SCXLNXCommSendReceive(): Message answer ready\n");
	return nError;

destroy_context:
	nError = SCXLNXCommSendMessage(pComm, pMessage, NULL, false);

	/*
	 * SCXLNXCommSendMessage cannot return an error because
	 * it's not killable and not within a connection
	 */
	BUG_ON(nError != 0);

	/* Reset the state, so a new CREATE DEVICE CONTEXT can be sent */
	spin_lock(&(pConn->stateLock));
	pConn->nState = SCXLNX_CONN_STATE_NO_DEVICE_CONTEXT;
	spin_unlock(&(pConn->stateLock));

	return nError;
}

/*----------------------------------------------------------------------------
 * Power management
 *----------------------------------------------------------------------------*/

/*
 * Perform a Secure World shutdown operation.
 * The routine does not return if the operation succeeds.
 * the routine returns an appropriate error code if
 * the operation fails.
 */
#if 0
static int SCXLNXCommShutdown(struct SCXLNX_COMM *pComm)
{
	int nError;
	union SCX_COMMAND_MESSAGE sMessage;
	union SCX_ANSWER_MESSAGE sAnswer;

	dprintk(KERN_INFO "SCXLNXCommShutdown()\n");

	sMessage.sHeader.nMessageType = SCX_MESSAGE_TYPE_POWER_MANAGEMENT;
	sMessage.sPowerManagementMessage.nPowerCommand = SCPM_PREPARE_SHUTDOWN;
	sMessage.sPowerManagementMessage.nSharedMemDescriptors[0] = 0;
	sMessage.sPowerManagementMessage.nSharedMemDescriptors[1] = 0;
	sMessage.sPowerManagementMessage.nSharedMemSize = 0;
	sMessage.sPowerManagementMessage.nSharedMemStartOffset = 0;

	nError = SCXLNXCommSendReceive(
		pComm,
		&sMessage,
		&sAnswer,
		NULL,
		false);

	if (nError != 0) {
		dprintk(KERN_ERR "SCXLNXCommShutdown(): "
			"SCXLNXCommSendReceive failed (error %d)!\n",
			nError);
		return nError;
	}

	printk(KERN_INFO "smodule: shut down.\n");

	return 0;
}
#endif

/*
 * Handles all the power management calls.
 * The nOperation is the type of power management
 * operation to be performed.
 *
 * This routine will only return if a failure occured or if
 * the required opwer management is of type "resume".
 * "Hibernate" and "Shutdown" should lock when doing the
 * corresponding SMC to the Secure World
 */
int SCXLNXCommPowerManagement(struct SCXLNX_COMM *pComm,
	enum SCXLNX_POWER_OPERATION nOperation)
{
	u32 nStatus;
	int nError = 0;

	dprintk(KERN_INFO "SCXLNXCommPowerManagement(%d)\n", nOperation);

#ifdef CONFIG_TF_MSHIELD
	if (!test_bit(SCXLNX_COMM_FLAG_PA_AVAILABLE, &pComm->nFlags)) {
		dprintk(KERN_INFO "SCXLNXCommPowerManagement(%p): "
			"succeeded (not started)\n", pComm);

		return 0;
	}
#endif

	nStatus = ((SCXLNXCommReadReg32(&(pComm->pBuffer->nStatus_S))
		& SCX_STATUS_POWER_STATE_MASK)
		>> SCX_STATUS_POWER_STATE_SHIFT);

	switch (nOperation) {
	case SCXLNX_POWER_OPERATION_SHUTDOWN:
		switch (nStatus) {
		case SCX_POWER_MODE_ACTIVE:
			#if 0
			nError = SCXLNXCommShutdown(pComm);
			#endif
			/* The SMC PA does not support this command
			   in this version */
			nError = 0;

			if (nError) {
				printk(KERN_ERR "SCXLNXCommPowerManagement(): "
					"Failed with error code 0x%08x\n",
					nError);
				goto error;
			}
			break;

		default:
			goto not_allowed;
		}
		break;

	case SCXLNX_POWER_OPERATION_HIBERNATE:
		switch (nStatus) {
		case SCX_POWER_MODE_ACTIVE:
			nError = SCXLNXCommHibernate(pComm);

			if (nError) {
				printk(KERN_ERR "SCXLNXCommPowerManagement(): "
					"Failed with error code 0x%08x\n",
					nError);
				goto error;
			}
			break;

		default:
			goto not_allowed;
		}
		break;

	case SCXLNX_POWER_OPERATION_RESUME:
		nError = SCXLNXCommResume(pComm);

		if (nError != 0) {
			printk(KERN_ERR "SCXLNXCommPowerManagement(): "
				"Failed with error code 0x%08x\n",
				nError);
			goto error;
		}
		break;
	}

	printk(KERN_INFO "SCXLNXCommPowerManagement(): succeeded\n");
	return 0;

not_allowed:
	printk(KERN_ERR "SCXLNXCommPowerManagement(): "
		"Power command not allowed in current "
		"Secure World state %d\n", nStatus);
	nError = -ENOTTY;
error:
	return nError;
}
