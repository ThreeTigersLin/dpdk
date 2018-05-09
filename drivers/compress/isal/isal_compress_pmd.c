/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2018 Intel Corporation
 */
#include <isa-l.h>

#include <rte_bus_vdev.h>
#include <rte_common.h>
#include <rte_malloc.h>
#include <rte_compressdev_pmd.h>

#include "isal_compress_pmd_private.h"

#define RTE_COMP_ISAL_WINDOW_SIZE 15
#define RTE_COMP_ISAL_LEVEL_ZERO 0 /* ISA-L Level 0 used for fixed Huffman */
#define RTE_COMP_ISAL_LEVEL_ONE 1
#define RTE_COMP_ISAL_LEVEL_TWO 2
#define RTE_COMP_ISAL_LEVEL_THREE 3 /* Optimised for AVX512 & AVX2 only */

int isal_logtype_driver;

/* Verify and set private xform parameters */
int
isal_comp_set_priv_xform_parameters(struct isal_priv_xform *priv_xform,
		const struct rte_comp_xform *xform)
{
	if (xform == NULL)
		return -EINVAL;

	/* Set compression private xform variables */
	if (xform->type == RTE_COMP_COMPRESS) {
		/* Set private xform type - COMPRESS/DECOMPRESS */
		priv_xform->type = RTE_COMP_COMPRESS;

		/* Set private xform algorithm */
		if (xform->compress.algo != RTE_COMP_ALGO_DEFLATE) {
			if (xform->compress.algo == RTE_COMP_ALGO_NULL) {
				ISAL_PMD_LOG(ERR, "By-pass not supported\n");
				return -ENOTSUP;
			}
			ISAL_PMD_LOG(ERR, "Algorithm not supported\n");
			return -ENOTSUP;
		}
		priv_xform->compress.algo = RTE_COMP_ALGO_DEFLATE;

		/* Set private xform checksum - raw deflate by default */
		if (xform->compress.chksum != RTE_COMP_CHECKSUM_NONE) {
			ISAL_PMD_LOG(ERR, "Checksum not supported\n");
			return -ENOTSUP;
		}

		/* Set private xform window size, 32K supported */
		if (xform->compress.window_size == RTE_COMP_ISAL_WINDOW_SIZE)
			priv_xform->compress.window_size =
					RTE_COMP_ISAL_WINDOW_SIZE;
		else {
			ISAL_PMD_LOG(ERR, "Window size not supported\n");
			return -ENOTSUP;
		}

		/* Set private xform huffman type */
		switch (xform->compress.deflate.huffman) {
		case(RTE_COMP_HUFFMAN_DEFAULT):
			priv_xform->compress.deflate.huffman =
					RTE_COMP_HUFFMAN_DEFAULT;
			break;
		case(RTE_COMP_HUFFMAN_FIXED):
			priv_xform->compress.deflate.huffman =
					RTE_COMP_HUFFMAN_FIXED;
			break;
		case(RTE_COMP_HUFFMAN_DYNAMIC):
			priv_xform->compress.deflate.huffman =
					RTE_COMP_HUFFMAN_DYNAMIC;
			break;
		default:
			ISAL_PMD_LOG(ERR, "Huffman code not supported\n");
			return -ENOTSUP;
		}

		/* Set private xform level.
		 * Checking compliance with compressdev API, -1 <= level => 9
		 */
		if (xform->compress.level < RTE_COMP_LEVEL_PMD_DEFAULT ||
				xform->compress.level > RTE_COMP_LEVEL_MAX) {
			ISAL_PMD_LOG(ERR, "Compression level out of range\n");
			return -EINVAL;
		}
		/* Check for Compressdev API level 0, No compression
		 * not supported in ISA-L
		 */
		else if (xform->compress.level == RTE_COMP_LEVEL_NONE) {
			ISAL_PMD_LOG(ERR, "No Compression not supported\n");
			return -ENOTSUP;
		}
		/* If using fixed huffman code, level must be 0 */
		else if (priv_xform->compress.deflate.huffman ==
				RTE_COMP_HUFFMAN_FIXED) {
			ISAL_PMD_LOG(DEBUG, "ISA-L level 0 used due to a"
					" fixed huffman code\n");
			priv_xform->compress.level = RTE_COMP_ISAL_LEVEL_ZERO;
			priv_xform->level_buffer_size =
					ISAL_DEF_LVL0_DEFAULT;
		} else {
			/* Mapping API levels to ISA-L levels 1,2 & 3 */
			switch (xform->compress.level) {
			case RTE_COMP_LEVEL_PMD_DEFAULT:
				/* Default is 1 if not using fixed huffman */
				priv_xform->compress.level =
						RTE_COMP_ISAL_LEVEL_ONE;
				priv_xform->level_buffer_size =
						ISAL_DEF_LVL1_DEFAULT;
				break;
			case RTE_COMP_LEVEL_MIN:
				priv_xform->compress.level =
						RTE_COMP_ISAL_LEVEL_ONE;
				priv_xform->level_buffer_size =
						ISAL_DEF_LVL1_DEFAULT;
				break;
			case RTE_COMP_ISAL_LEVEL_TWO:
				priv_xform->compress.level =
						RTE_COMP_ISAL_LEVEL_TWO;
				priv_xform->level_buffer_size =
						ISAL_DEF_LVL2_DEFAULT;
				break;
			/* Level 3 or higher requested */
			default:
				/* Check for AVX512, to use ISA-L level 3 */
				if (rte_cpu_get_flag_enabled(
						RTE_CPUFLAG_AVX512F)) {
					priv_xform->compress.level =
						RTE_COMP_ISAL_LEVEL_THREE;
					priv_xform->level_buffer_size =
						ISAL_DEF_LVL3_DEFAULT;
				}
				/* Check for AVX2, to use ISA-L level 3 */
				else if (rte_cpu_get_flag_enabled(
						RTE_CPUFLAG_AVX2)) {
					priv_xform->compress.level =
						RTE_COMP_ISAL_LEVEL_THREE;
					priv_xform->level_buffer_size =
						ISAL_DEF_LVL3_DEFAULT;
				} else {
					ISAL_PMD_LOG(DEBUG, "Requested ISA-L level"
						" 3 or above; Level 3 optimized"
						" for AVX512 & AVX2 only."
						" level changed to 2.\n");
					priv_xform->compress.level =
						RTE_COMP_ISAL_LEVEL_TWO;
					priv_xform->level_buffer_size =
						ISAL_DEF_LVL2_DEFAULT;
				}
			}
		}
	}

	/* Set decompression private xform variables */
	else if (xform->type == RTE_COMP_DECOMPRESS) {

		/* Set private xform type - COMPRESS/DECOMPRESS */
		priv_xform->type = RTE_COMP_DECOMPRESS;

		/* Set private xform algorithm */
		if (xform->decompress.algo != RTE_COMP_ALGO_DEFLATE) {
			if (xform->decompress.algo == RTE_COMP_ALGO_NULL) {
				ISAL_PMD_LOG(ERR, "By pass not supported\n");
				return -ENOTSUP;
			}
			ISAL_PMD_LOG(ERR, "Algorithm not supported\n");
			return -ENOTSUP;
		}
		priv_xform->decompress.algo = RTE_COMP_ALGO_DEFLATE;

		/* Set private xform checksum - raw deflate by default */
		if (xform->compress.chksum != RTE_COMP_CHECKSUM_NONE) {
			ISAL_PMD_LOG(ERR, "Checksum not supported\n");
			return -ENOTSUP;
		}

		/* Set private xform window size, 32K supported */
		if (xform->decompress.window_size == RTE_COMP_ISAL_WINDOW_SIZE)
			priv_xform->decompress.window_size =
					RTE_COMP_ISAL_WINDOW_SIZE;
		else {
			ISAL_PMD_LOG(ERR, "Window size not supported\n");
			return -ENOTSUP;
		}
	}
	return 0;
}

/* Create ISA-L compression device */
static int
compdev_isal_create(const char *name, struct rte_vdev_device *vdev,
		struct rte_compressdev_pmd_init_params *init_params)
{
	struct rte_compressdev *dev;

	dev = rte_compressdev_pmd_create(name, &vdev->device,
			sizeof(struct isal_comp_private), init_params);
	if (dev == NULL) {
		ISAL_PMD_LOG(ERR, "failed to create compressdev vdev");
		return -EFAULT;
	}

	dev->dev_ops = isal_compress_pmd_ops;

	return 0;
}

/** Remove compression device */
static int
compdev_isal_remove_dev(struct rte_vdev_device *vdev)
{
	struct rte_compressdev *compdev;
	const char *name;

	name = rte_vdev_device_name(vdev);
	if (name == NULL)
		return -EINVAL;

	compdev = rte_compressdev_pmd_get_named_dev(name);
	if (compdev == NULL)
		return -ENODEV;

	return rte_compressdev_pmd_destroy(compdev);
}

/** Initialise ISA-L compression device */
static int
compdev_isal_probe(struct rte_vdev_device *dev)
{
	struct rte_compressdev_pmd_init_params init_params = {
		"",
		rte_socket_id(),
	};
	const char *name, *args;
	int retval;

	name = rte_vdev_device_name(dev);
	if (name == NULL)
		return -EINVAL;

	args = rte_vdev_device_args(dev);

	retval = rte_compressdev_pmd_parse_input_args(&init_params, args);
	if (retval) {
		ISAL_PMD_LOG(ERR,
			"Failed to parse initialisation arguments[%s]\n", args);
		return -EINVAL;
	}

	return compdev_isal_create(name, dev, &init_params);
}

static struct rte_vdev_driver compdev_isal_pmd_drv = {
	.probe = compdev_isal_probe,
	.remove = compdev_isal_remove_dev,
};

RTE_PMD_REGISTER_VDEV(COMPDEV_NAME_ISAL_PMD, compdev_isal_pmd_drv);
RTE_PMD_REGISTER_PARAM_STRING(COMPDEV_NAME_ISAL_PMD,
	"socket_id=<int>");

RTE_INIT(isal_init_log);

static void
isal_init_log(void)
{
	isal_logtype_driver = rte_log_register("comp_isal");
	if (isal_logtype_driver >= 0)
		rte_log_set_level(isal_logtype_driver, RTE_LOG_INFO);
}
