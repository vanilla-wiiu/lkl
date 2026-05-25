// SPDX-License-Identifier: GPL-2.0

#include <kunit/test.h>

#include <linux/dma-mapping.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/page_ref.h>
#include <linux/pci.h>

#include <asm/host_ops.h>

#define LKL_DMA_TEST_HANDLE	0x4c4b4cULL

struct lkl_dma_test_state {
	struct lkl_pci_dev *mapped_dev;
	void *mapped_vaddr;
	unsigned long mapped_size;
	unsigned int map_calls;
	struct lkl_pci_dev *unmapped_dev;
	unsigned long long unmapped_handle;
	unsigned long unmapped_size;
	unsigned int unmap_calls;
};

static struct lkl_dma_test_state *lkl_dma_test_state;

static unsigned long long lkl_dma_test_map_page(struct lkl_pci_dev *dev,
						void *vaddr,
						unsigned long size)
{
	lkl_dma_test_state->mapped_dev = dev;
	lkl_dma_test_state->mapped_vaddr = vaddr;
	lkl_dma_test_state->mapped_size = size;
	lkl_dma_test_state->map_calls++;

	return LKL_DMA_TEST_HANDLE;
}

static void lkl_dma_test_unmap_page(struct lkl_pci_dev *dev,
				    unsigned long long dma_handle,
				    unsigned long size)
{
	lkl_dma_test_state->unmapped_dev = dev;
	lkl_dma_test_state->unmapped_handle = dma_handle;
	lkl_dma_test_state->unmapped_size = size;
	lkl_dma_test_state->unmap_calls++;
}

static struct lkl_dev_pci_ops lkl_dma_test_pci_ops = {
	.map_page = lkl_dma_test_map_page,
	.unmap_page = lkl_dma_test_unmap_page,
};

static void lkl_dma_alloc_free_test(struct kunit *test)
{
	struct lkl_dev_pci_ops *saved_pci_ops;
	struct lkl_dma_test_state state = {};
	struct pci_dev pdev = {};
	dma_addr_t dma_handle;
	struct page *page;
	void *cpu_addr;

	KUNIT_ASSERT_NOT_ERR_OR_NULL(test, lkl_ops);

	pdev.sysdata = &state;
	pdev.dma_mask = DMA_BIT_MASK(64);
	pdev.dev.dma_mask = &pdev.dma_mask;
	pdev.dev.coherent_dma_mask = DMA_BIT_MASK(64);

	lkl_dma_test_state = &state;
	saved_pci_ops = lkl_ops->pci_ops;
	lkl_ops->pci_ops = &lkl_dma_test_pci_ops;

	cpu_addr = dma_alloc_attrs(&pdev.dev, PAGE_SIZE, &dma_handle,
				   GFP_KERNEL, 0);
	KUNIT_EXPECT_NOT_ERR_OR_NULL(test, cpu_addr);
	if (!cpu_addr)
		goto out_restore;

	page = virt_to_page(cpu_addr);
	KUNIT_EXPECT_EQ(test, page_count(page), 1);
	KUNIT_EXPECT_EQ(test, state.map_calls, 1);
	KUNIT_EXPECT_PTR_EQ(test, state.mapped_dev,
			    (struct lkl_pci_dev *)pdev.sysdata);
	KUNIT_EXPECT_PTR_EQ(test, state.mapped_vaddr, cpu_addr);
	KUNIT_EXPECT_EQ(test, state.mapped_size, PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, dma_handle, (dma_addr_t)LKL_DMA_TEST_HANDLE);

	dma_free_attrs(&pdev.dev, PAGE_SIZE, cpu_addr, dma_handle, 0);

	KUNIT_EXPECT_EQ(test, page_count(page), 0);
	KUNIT_EXPECT_EQ(test, state.unmap_calls, 1);
	KUNIT_EXPECT_PTR_EQ(test, state.unmapped_dev,
			    (struct lkl_pci_dev *)pdev.sysdata);
	KUNIT_EXPECT_EQ(test, state.unmapped_handle, LKL_DMA_TEST_HANDLE);
	KUNIT_EXPECT_EQ(test, state.unmapped_size, PAGE_SIZE);

out_restore:
	lkl_ops->pci_ops = saved_pci_ops;
	lkl_dma_test_state = NULL;
}

static struct kunit_case lkl_pci_kunit_test_cases[] = {
	KUNIT_CASE(lkl_dma_alloc_free_test),
	{}
};

static struct kunit_suite lkl_pci_kunit_test_suite = {
	.name = "lkl_pci",
	.test_cases = lkl_pci_kunit_test_cases,
};

kunit_test_suite(lkl_pci_kunit_test_suite);

MODULE_LICENSE("GPL");
