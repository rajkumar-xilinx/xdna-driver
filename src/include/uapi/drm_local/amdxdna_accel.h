/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2022-2025, Advanced Micro Devices, Inc.
 */

#ifndef AMDXDNA_ACCEL_H_
#define AMDXDNA_ACCEL_H_

#ifdef __KERNEL__
#include <drm/drm.h>
#else
#include <libdrm/drm.h>
#endif
#include <linux/const.h>
#include <linux/stddef.h>

#if defined(__cplusplus)
extern "C" {
#endif

#define AMDXDNA_DRIVER_MAJOR		1
#define AMDXDNA_DRIVER_MINOR		0

#define AMDXDNA_INVALID_ADDR		(~0UL)
#define AMDXDNA_INVALID_CTX_HANDLE	0
#define AMDXDNA_INVALID_BO_HANDLE	0
#define AMDXDNA_INVALID_FENCE_HANDLE	0

#define POWER_MODE_DEFAULT	0
#define POWER_MODE_LOW		1
#define POWER_MODE_MEDIUM	2
#define POWER_MODE_HIGH		3
#define POWER_MODE_TURBO	4

/*
 * The interface can grow/extend over time.
 * On each struct amdxdna_drm_*, to support potential extension, we defined it
 * like this.
 *
 * Example code:
 *
 * struct amdxdna_drm_example_data {
 *	.ext = (uintptr_t)&example_data_ext;
 *	...
 * };
 *
 * We don't have extension now. The extension struct will define in the future.
 */

#define	DRM_AMDXDNA_CREATE_HWCTX		0
#define	DRM_AMDXDNA_DESTROY_HWCTX		1
#define	DRM_AMDXDNA_CONFIG_HWCTX		2
#define	DRM_AMDXDNA_CREATE_BO		3
#define	DRM_AMDXDNA_GET_BO_INFO		4
#define	DRM_AMDXDNA_SYNC_BO		5
#define	DRM_AMDXDNA_EXEC_CMD		6
#define	DRM_AMDXDNA_GET_INFO		7
#define	DRM_AMDXDNA_SET_STATE		8
#define	DRM_AMDXDNA_WAIT_CMD		9
#define DRM_AMDXDNA_GET_INFO_ARRAY	10

#define	AMDXDNA_DEV_TYPE_UNKNOWN	-1
#define	AMDXDNA_DEV_TYPE_KMQ		0
#define	AMDXDNA_DEV_TYPE_UMQ		1

/*
 * Define priority in application's QoS.
 * AMDXDNA_QOS_REALTIME_PRIORITY: Real time clients.
 * AMDXDNA_QOS_HIGH_PRIORITY: Best effort foreground clients.
 * AMDXDNA_QOS_NORMAL_PRIORITY: Best effort or background clients.
 * AMDXDNA_QOS_LOW_PRIORITY: Clients that can wait indefinite amount of time for
 *                           completion.
 *
 * NOTE, if driver see value beyond above definition, it decides the priority of
 * the context without error/warning.
 */
#define	AMDXDNA_QOS_REALTIME_PRIORITY	0x100
#define	AMDXDNA_QOS_HIGH_PRIORITY	0x180
#define	AMDXDNA_QOS_NORMAL_PRIORITY	0x200
#define	AMDXDNA_QOS_LOW_PRIORITY	0x280
/* The maximum number of priority */
#define	AMDXDNA_NUM_PRIORITY		4

/**
 * struct qos_info - QoS information for driver.
 * @gops: Giga operations per workload.
 * @fps: Workload per second.
 * @dma_bandwidth: DMA bandwidtha.
 * @latency: Frame response latency.
 * @frame_exec_time: Frame execution time.
 * @priority: Request priority.
 *
 * User program can provide QoS hints to driver.
 */
struct amdxdna_qos_info {
	__u32 gops;
	__u32 fps;
	__u32 dma_bandwidth;
	__u32 latency;
	__u32 frame_exec_time;
	__u32 priority;
};

/**
 * struct amdxdna_drm_create_hwctx - Create context.
 * @ext: MBZ.
 * @ext_flags: MBZ.
 * @qos_p: Address of QoS info.
 * @umq_bo: BO handle for user mode queue(UMQ).
 * @log_buf_bo: BO handle for log buffer.
 * @max_opc: Maximum operations per cycle.
 * @num_tiles: Number of AIE tiles.
 * @mem_size: Size of AIE tile memory.
 * @umq_doorbell: Returned offset of doorbell associated with UMQ.
 * @handle: Returned context handle.
 * @syncobj_handle: The drm timeline syncobj handle for command completion notification.
 */
struct amdxdna_drm_create_hwctx {
	__u64 ext;
	__u64 ext_flags;
	__u64 qos_p;
	__u32 umq_bo;
	__u32 log_buf_bo;
	__u32 max_opc;
	__u32 num_tiles;
	__u32 mem_size;
	__u32 umq_doorbell;
	__u32 handle;
	__u32 syncobj_handle;
};

/**
 * struct amdxdna_drm_destroy_hwctx - Destroy context.
 * @handle: Context handle.
 * @pad: Structure padding.
 */
struct amdxdna_drm_destroy_hwctx {
	__u32 handle;
	__u32 pad;
};

/**
 * struct amdxdna_cu_config - configuration for one CU
 * @cu_bo: CU configuration buffer bo handle.
 * @cu_func: Function of a CU.
 * @pad: Structure padding.
 */
struct amdxdna_cu_config {
	__u32 cu_bo;
	__u8  cu_func;
	__u8  pad[3];
};

/**
 * struct amdxdna_hwctx_param_config_cu - configuration for CUs in context
 * @num_cus: Number of CUs to configure.
 * @pad: Structure padding.
 * @cu_configs: Array of CU configurations of struct amdxdna_cu_config.
 */
struct amdxdna_hwctx_param_config_cu {
	__u16 num_cus;
	__u16 pad[3];
	struct amdxdna_cu_config cu_configs[];
};

/**
 * struct uc_info_entry: Holds uc index & buffer size allotment info
 * @index: uc index
 *     On aie2ps, uc index is same to column index
 *     On aie4, uc index is mapped as 0->0_A, 1->0_B, 2->1_A, 3->1_B, 4->2_A, 5->2_B
 * @size: buffer size in bytes for this uc
 */
struct uc_info_entry {
	__u32 index;
	__u32 size;
};

/**
 * struct fw_buffer_metadata - Holds buffer configuration.
 * @buf_type: buffer type set to fw
 * @num_ucs: total ucs to config
 * @command_id: command id used for trace
 * @bo_handle: actual bo handle
 * @uc_info_entry: uc index & buffer size mapping info
 */
struct fw_buffer_metadata {
#define AMDXDNA_FW_BUF_DEBUG	0
#define AMDXDNA_FW_BUF_TRACE	1
#define AMDXDNA_FW_BUF_DBG_Q	2
#define AMDXDNA_FW_BUF_LOG	3
	__u8 buf_type;
	__u8 num_ucs;
	__u8 pad[48];
	__u64 command_id;
	__u64 bo_handle;
	struct uc_info_entry uc_info[];
};

/**
 * struct amdxdna_drm_config_hwctx - Configure context.
 * @handle: Context handle.
 * @param_type: Specifies the structure passed in via param_val.
 * @param_val: A structure specified by the param_type struct member.
 * @param_val_size: Size of the parameter buffer pointed to by the param_val.
 *		    If param_val is not a pointer, driver can ignore this.
 * @pad: Structure padding.
 *
 * Note: if the param_val is a pointer pointing to a buffer, the maximum size
 * of the buffer is 4KiB(PAGE_SIZE).
 */
struct amdxdna_drm_config_hwctx {
	__u32 handle;
#define DRM_AMDXDNA_HWCTX_CONFIG_CU		0
#define DRM_AMDXDNA_HWCTX_ASSIGN_DBG_BUF	1
#define DRM_AMDXDNA_HWCTX_REMOVE_DBG_BUF	2
#define DRM_AMDXDNA_HWCTX_CONFIG_OPCODE_TIMEOUT	3
	__u32 param_type;
	__u64 param_val;
	__u32 param_val_size;
	__u32 pad;
};

/**
 * struct amdxdna_drm_va_entry
 * @vaddr: Virtual address.
 * @len: Size of entry.
 */
struct amdxdna_drm_va_entry {
	__u64 vaddr;
	__u64 len;
};

/**
 * struct amdxdna_drm_va_tbl
 * @udma_fd: UDMABUF fd.
 * @num_entries: Number of va entries.
 * @va_entries: Array of va entries.
 */
struct amdxdna_drm_va_tbl {
	__s32 udma_fd;
	__u32 num_entries;
	struct amdxdna_drm_va_entry va_entries[];
};

/**
 * struct amdxdna_drm_create_bo - Create a buffer object.
 * @flags: Buffer flags. MBZ.
 * @vaddr: Pointer of va address table.
 * @size: Size in bytes.
 * @type: Buffer type.
 * @handle: Returned DRM buffer object handle.
 */
struct amdxdna_drm_create_bo {
	__u64	flags;
	__u64	vaddr;
	__u64	size;
#define	AMDXDNA_BO_INVALID	0 /* Invalid BO type */
#define	AMDXDNA_BO_SHARE	1 /* Regular BO shared between user and device */
#define	AMDXDNA_BO_DEV_HEAP	2 /* Shared host memory to device as heap memory */
#define	AMDXDNA_BO_DEV		3 /* Allocated from BO_DEV_HEAP */
#define	AMDXDNA_BO_CMD		4 /* User and driver accessible BO */
#define	AMDXDNA_BO_DMA		5 /* DRM GEM DMA BO */
	__u32	type;
	__u32	handle;
};

/**
 * struct amdxdna_drm_get_bo_info - Get buffer object information.
 * @ext: MBZ.
 * @ext_flags: MBZ.
 * @handle: DRM buffer object handle.
 * @pad: Structure padding.
 * @map_offset: Returned DRM fake offset for mmap().
 * @vaddr: Returned user VA of buffer. 0 in case user needs mmap().
 * @xdna_addr: Returned XDNA device virtual address.
 */
struct amdxdna_drm_get_bo_info {
	__u64 ext;
	__u64 ext_flags;
	__u32 handle;
	__u32 pad;
	__u64 map_offset;
	__u64 vaddr;
	__u64 xdna_addr;
};

/**
 * struct amdxdna_drm_sync_bo - Sync buffer object.
 * @handle: Buffer object handle.
 * @direction: Direction of sync, can be from device or to device.
 * @offset: Offset in the buffer to sync.
 * @size: Size in bytes.
 */
struct amdxdna_drm_sync_bo {
	__u32 handle;
#define SYNC_DIRECT_TO_DEVICE	0U
#define SYNC_DIRECT_FROM_DEVICE	1U
	__u32 direction;
	__u64 offset;
	__u64 size;
};

/**
 * struct amdxdna_drm_exec_cmd - Execute command.
 * @ext: MBZ.
 * @ext_flags: MBZ.
 * @ctx: Context handle.
 * @type: Command type.
 * @cmd_handles: Array of command handles or the command handle itself
 *               in case of just one.
 * @args: Array of arguments for all command handles.
 * @cmd_count: Number of command handles in the cmd_handles array.
 * @arg_count: Number of arguments in the args array.
 * @seq: Returned sequence number for this command.
 */
struct amdxdna_drm_exec_cmd {
	__u64 ext;
	__u64 ext_flags;
	__u32 hwctx;
#define	AMDXDNA_CMD_SUBMIT_EXEC_BUF	0
#define	AMDXDNA_CMD_SUBMIT_DEPENDENCY	1
#define	AMDXDNA_CMD_SUBMIT_SIGNAL	2
	__u32 type;
	__u64 cmd_handles;
	__u64 args;
	__u32 cmd_count;
	__u32 arg_count;
	__u64 seq;
};

/**
 * struct amdxdna_drm_wait_cmd - Wait exectuion command.
 *
 * @ctx: Context handle.
 * @timeout: timeout in ms, 0 implies infinite wait.
 * @seq: sequence number of the command returned by execute command.
 *
 * Wait a command specified by seq to be completed.
 */
struct amdxdna_drm_wait_cmd {
	__u32 hwctx;
	__u32 timeout;
	__u64 seq;
};

/**
 * struct amdxdna_drm_query_aie_status - Query the status of the AIE hardware
 * @buffer: The user space buffer that will return the AIE status.
 * @buffer_size: The size of the user space buffer.
 * @cols_filled: A bitmap of AIE columns whose data has been returned in the buffer.
 */
struct amdxdna_drm_query_aie_status {
	__u64 buffer; /* out */
	__u32 buffer_size; /* in */
	__u32 cols_filled; /* out */
};

/**
 * struct amdxdna_drm_query_aie_version - Query the version of the AIE hardware
 * @major: The major version number.
 * @minor: The minor version number.
 */
struct amdxdna_drm_query_aie_version {
	__u32 major; /* out */
	__u32 minor; /* out */
};

/**
 * struct amdxdna_drm_query_aie_tile_metadata - Query the metadata of AIE tile (core, mem, shim)
 * @row_count: The number of rows.
 * @row_start: The starting row number.
 * @dma_channel_count: The number of dma channels.
 * @lock_count: The number of locks.
 * @event_reg_count: The number of events.
 * @pad: Structure padding.
 */
struct amdxdna_drm_query_aie_tile_metadata {
	__u16 row_count;
	__u16 row_start;
	__u16 dma_channel_count;
	__u16 lock_count;
	__u16 event_reg_count;
	__u16 pad[3];
};

/**
 * struct amdxdna_drm_query_aie_metadata - Query the metadata of the AIE hardware
 * @col_size: The size of a column in bytes.
 * @cols: The total number of columns.
 * @rows: The total number of rows.
 * @version: The version of the AIE hardware.
 * @core: The metadata for all core tiles.
 * @mem: The metadata for all mem tiles.
 * @shim: The metadata for all shim tiles.
 */
struct amdxdna_drm_query_aie_metadata {
	__u32 col_size;
	__u16 cols;
	__u16 rows;
	struct amdxdna_drm_query_aie_version version;
	struct amdxdna_drm_query_aie_tile_metadata core;
	struct amdxdna_drm_query_aie_tile_metadata mem;
	struct amdxdna_drm_query_aie_tile_metadata shim;
};

/**
 * struct amdxdna_drm_query_clock - Metadata for a clock
 * @name: The clock name.
 * @freq_mhz: The clock frequency.
 * @pad: Structure padding.
 */
struct amdxdna_drm_query_clock {
	__u8 name[16];
	__u32 freq_mhz;
	__u32 pad;
};

/**
 * struct amdxdna_drm_query_clock_metadata - Query metadata for clocks
 * @mp_npu_clock: The metadata for MP-NPU clock.
 * @h_clock: The metadata for H clock.
 */
struct amdxdna_drm_query_clock_metadata {
	struct amdxdna_drm_query_clock mp_npu_clock;
	struct amdxdna_drm_query_clock h_clock;
};

/**
 * struct amdxdna_drm_query_sensor - The data for single sensor.
 * @label: The name for a sensor.
 * @input: The current value of the sensor.
 * @max: The maximum value possible for the sensor.
 * @average: The average value of the sensor.
 * @highest: The highest recorded sensor value for this driver load for the sensor.
 * @status: The sensor status.
 * @units: The sensor units.
 * @unitm: Translates value member variables into the correct unit via (pow(10, unitm) * value).
 * @type: The sensor type.
 * @pad: Structure padding.
 */
struct amdxdna_drm_query_sensor {
	__u8  label[64];
	__u32 input;
	__u32 max;
	__u32 average;
	__u32 highest;
	__u8  status[64];
	__u8  units[16];
	__s8  unitm;
#define AMDXDNA_SENSOR_TYPE_POWER 0
	__u8  type;
	__u8  pad[6];
};

/**
 * struct amdxdna_drm_query_hwctx - The data for single context.
 * @context_id: The ID for this context.
 * @start_col: The starting column for the partition assigned to this context.
 * @num_col: The number of columns in the partition assigned to this context.
 * @pad: Structure padding.
 * @pid: The Process ID of the process that created this context.
 * @command_submissions: The number of commands submitted to this context.
 * @command_completions: The number of commands completed by this context.
 * @migrations: The number of times this context has been moved to a different partition.
 * @preemptions: The number of times this context has been preempted by another context in the
 *               same partition.
 * @errors: The errors for this context.
 *
 * !!! NOTE: Never expand this struct. Use amdxdna_drm_query_hwctx_array instead. !!!
 */
struct amdxdna_drm_query_hwctx {
	__u32 context_id;
	__u32 start_col;
	__u32 num_col;
	__u32 pad;
	__s64 pid;
	__u64 command_submissions;
	__u64 command_completions;
	__u64 migrations;
	__u64 preemptions;
	__u64 errors;
};

/**
 * struct amdxdna_drm_aie_mem - The data for AIE memory read/write
 * @col:   The AIE column index
 * @row:   The AIE row index
 * @addr:  The AIE memory address to read/write
 * @size:  The size of bytes to read/write
 * @buf_p: The buffer to store read/write data
 *
 * This is used for DRM_AMDXDNA_READ_AIE_MEM and DRM_AMDXDNA_WRITE_AIE_MEM
 * parameters.
 */
struct amdxdna_drm_aie_mem {
	__u32 col;
	__u32 row;
	__u32 addr;
	__u32 size;
	__u64 buf_p;
};

/**
 * struct amdxdna_drm_aie_reg - The data for AIE register read/write
 * @col: The AIE column index
 * @row: The AIE row index
 * @addr: The AIE register address to read/write
 * @val: The value to write or returned value from AIE
 *
 * This is used for DRM_AMDXDNA_READ_AIE_REG and DRM_AMDXDNA_WRITE_AIE_REG
 * parameters.
 */
struct amdxdna_drm_aie_reg {
	__u32 col;
	__u32 row;
	__u32 addr;
	__u32 val;
};

/**
 * struct amdxdna_drm_get_power_mode - Get the power mode of the AIE hardware
 * @power_mode: Returned current power mode
 * @pad: MBZ.
 */
struct amdxdna_drm_get_power_mode {
	__u8 power_mode;
	__u8 pad[7];
};

/**
 * struct amdxdna_drm_query_firmware_version - Query the version of the firmware
 * @major: The major version number
 * @minor: The minor version number
 * @patch: The patch level version number
 * @build: The build ID
 */
struct amdxdna_drm_query_firmware_version {
	__u32 major; /* out */
	__u32 minor; /* out */
	__u32 patch; /* out */
	__u32 build; /* out */
};

/**
 * struct amdxdna_drm_query_ve2_firmware_version - Query the git hash and version of the firmware
 * @major:  Major version number
 * @minor:  Minor version number
 * @date:  Build date of the firmware
 * @git_hash:  Git commit ID used to build the firmware version
 */
struct amdxdna_drm_query_ve2_firmware_version {
	__u8 major;
	__u8 minor;
	__u8 date[14];
	__u8 git_hash[48];
};

/**
 * struct amdxdna_drm_get_resource_info - Get info on some resources within NPU
 * @npu_clk_max: max H-Clocks
 * @npu_tops_max: max TOPs
 * @npu_task_max: max number of tasks
 * @npu_tops_curr: current TOPs
 * @npu_task_curr: current number of tasks
 */
struct amdxdna_drm_get_resource_info {
	__u64 npu_clk_max;
	__u64 npu_tops_max;
	__u64 npu_task_max;
	__u64 npu_tops_curr;
	__u64 npu_task_curr;
};

/**
 * struct amdxdna_drm_attribute_state - Represent buffer packing for the below
 *					struct amdxdna_drm_<get/set>_state attributes,
 *					Get:
 *						DRM_AMDXDNA_GET_FORCE_PREEMPT_STATE
 *						DRM_AMDXDNA_GET_FRAME_BOUNDARY_PREEMPT_STATE
 *					Set:
 *						DRM_AMDXDNA_SET_FORCE_PREEMPT
 *						DRM_AMDXDNA_SET_FRAME_BOUNDARY_PREEMPT
 * @state: 1 implies enabled/true. 0 implies disabled/false. Other value is invalid.
 * @pad: MBZ.
 */
struct amdxdna_drm_attribute_state {
	__u8 state;
	__u8 pad[7];
};

/**
 * struct amdxdna_drm_query_telemetry_header - Telemetry header to capture information shared
 *					       between driver and shim. Followed by the telemetry
 *					       data harvested from the firmware.
 * @major: Firmware telemetry interface major version number. Based on firmware response message.
 * @minor: Firmware telemetry interface minor version number. Based on firmware response message.
 * @type: Telemetry query type. Set by the user.
 *	  MBZ for NPU 1, 2, 4, 5, and 6. Non-zero for future generations.
 * @map_num_elements: Total number of elements in the map table. Set by the driver.
 * @map: Maps the firmware allocated context ID(key) to driver allocated context ID(value).
 */
struct amdxdna_drm_query_telemetry_header {
	__u32 major;
	__u32 minor;
	__u32 type;
	__u32 map_num_elements;
	__u32 map[];
};

/**
 * struct amdxdna_drm_get_info - Get some information from the AIE hardware.
 * @param: Specifies the structure passed in the buffer.
 * @buffer_size: Size of the input buffer. Size needed/written by the kernel.
 * @buffer: A structure specified by the param struct member.
 */
struct amdxdna_drm_get_info {
#define	DRM_AMDXDNA_QUERY_AIE_STATUS			0
#define	DRM_AMDXDNA_QUERY_AIE_METADATA			1
#define	DRM_AMDXDNA_QUERY_AIE_VERSION			2
#define	DRM_AMDXDNA_QUERY_CLOCK_METADATA		3
#define	DRM_AMDXDNA_QUERY_SENSORS			4
#define	DRM_AMDXDNA_QUERY_HW_CONTEXTS			5
#define	DRM_AMDXDNA_READ_AIE_MEM			6
#define	DRM_AMDXDNA_READ_AIE_REG			7
#define	DRM_AMDXDNA_QUERY_FIRMWARE_VERSION		8
#define	DRM_AMDXDNA_GET_POWER_MODE			9
#define	DRM_AMDXDNA_QUERY_TELEMETRY			10
#define	DRM_AMDXDNA_GET_FORCE_PREEMPT_STATE		11
#define	DRM_AMDXDNA_QUERY_RESOURCE_INFO			12
#define	DRM_AMDXDNA_GET_FRAME_BOUNDARY_PREEMPT_STATE	13
#define	DRM_AMDXDNA_QUERY_VE2_FIRMWARE_VERSION		14
	__u32 param; /* in */
	__u32 buffer_size; /* in/out */
	__u64 buffer; /* in/out */
};

/**
 * struct amdxdna_drm_query_hwctx_array - The element of a context in array
 * @context_id: The ID for this context.
 * @start_col: The starting column for the partition assigned to this context.
 * @num_col: The number of columns in the partition assigned to this context.
 * @hwctx_id: Hardware context ID.
 * @pid: The Process ID of the process that created this context.
 * @command_submissions: The number of commands submitted to this context.
 * @command_completions: The number of commands completed by this context.
 * @migrations: The number of times this context has been moved to a different partition.
 * @preemptions: The number of times this context has been preempted by another context in the
 *               same partition.
 * @errors: The errors for this context.
 * @priority: Context priority.
 * @heap_usage: The usage of heap buffer of the process
 * @suspensions: Context suspension count
 * @state: The state of context.
 * @pasid: PASID for this process
 * @gops: Giga operations per second
 * @fps: Frames per second
 * @dma_bandwidth: DMA bandwidth
 * @latency: Frame response latency
 * @frame_exec_time: Frame execution time
 */
struct amdxdna_drm_query_hwctx_array {
	__u32 context_id;
	__u32 start_col;
	__u32 num_col;
	__u32 hwctx_id;
	__s64 pid;
	__u64 command_submissions;
	__u64 command_completions;
	__u64 migrations;
	__u64 preemptions;
	__u64 errors;
	__u64 priority;
	__u64 heap_usage;
	__u64 suspensions;
#define AMDXDNA_HWCTX_STATE_IDLE	0
#define AMDXDNA_HWCTX_STATE_ACTIVE	1
	__u32 state;
	__u32 pasid;
	__u32 gops;
	__u32 fps;
	__u32 dma_bandwidth;
	__u32 latency;
	__u32 frame_exec_time;
};

/**
 * struct amdxdna_drm_get_info_array - Get some information from the AIE hardware, return array.
 * @param: Specifies the structure passed in the buffer.
 * @element_size: Size of each element in the array.
 * @num_element: The number of elements.
 * @buffer: Pointer to an array whose elements are structure specified by the param struct member.
 */
struct amdxdna_drm_get_info_array {
#define DRM_AMDXDNA_QUERY_HW_CONTEXTS_ARRAY	0
	__u32 param; /* in */
	__u32 element_size; /* in/out */
#define AMDXDNA_MAX_NUM_ELEMENT			1024
	__u32 num_element; /* in/out */
	__u32 pad;
	__u64 buffer; /* in/out */
};

/**
 * struct amdxdna_drm_set_power_mode - Set the power mode of the AIE hardware
 * @power_mode: The target power mode to be set
 * @pad: MBZ.
 */
struct amdxdna_drm_set_power_mode {
	__u8 power_mode;
	__u8 pad[7];
};

/**
 * struct amdxdna_drm_set_state - Set the state of some component within the AIE hardware.
 * @param: Specifies the structure passed in the buffer.
 * @buffer_size: Size of the input buffer.
 * @buffer: A structure specified by the param struct member.
 */
struct amdxdna_drm_set_state {
#define	DRM_AMDXDNA_SET_POWER_MODE		0
#define	DRM_AMDXDNA_WRITE_AIE_MEM		1
#define	DRM_AMDXDNA_WRITE_AIE_REG		2
#define	DRM_AMDXDNA_SET_FORCE_PREEMPT		3
#define	DRM_AMDXDNA_SET_FRAME_BOUNDARY_PREEMPT	4
	__u32 param; /* in */
	__u32 buffer_size; /* in */
	__u64 buffer; /* in */
};

#define DRM_IOCTL_AMDXDNA_CREATE_HWCTX \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDXDNA_CREATE_HWCTX, \
		 struct amdxdna_drm_create_hwctx)

#define DRM_IOCTL_AMDXDNA_DESTROY_HWCTX \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDXDNA_DESTROY_HWCTX, \
		 struct amdxdna_drm_destroy_hwctx)

#define DRM_IOCTL_AMDXDNA_CONFIG_HWCTX \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDXDNA_CONFIG_HWCTX, \
		 struct amdxdna_drm_config_hwctx)

#define DRM_IOCTL_AMDXDNA_CREATE_BO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDXDNA_CREATE_BO, \
		 struct amdxdna_drm_create_bo)

#define DRM_IOCTL_AMDXDNA_GET_BO_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDXDNA_GET_BO_INFO, \
		 struct amdxdna_drm_get_bo_info)

#define DRM_IOCTL_AMDXDNA_SYNC_BO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDXDNA_SYNC_BO, \
		 struct amdxdna_drm_sync_bo)

#define DRM_IOCTL_AMDXDNA_EXEC_CMD \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDXDNA_EXEC_CMD, \
		 struct amdxdna_drm_exec_cmd)

#define DRM_IOCTL_AMDXDNA_WAIT_CMD \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDXDNA_WAIT_CMD, \
		 struct amdxdna_drm_wait_cmd)

#define DRM_IOCTL_AMDXDNA_GET_INFO \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDXDNA_GET_INFO, \
		 struct amdxdna_drm_get_info)

#define DRM_IOCTL_AMDXDNA_GET_INFO_ARRAY \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDXDNA_GET_INFO_ARRAY, \
		struct amdxdna_drm_get_info_array)

#define DRM_IOCTL_AMDXDNA_SET_STATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDXDNA_SET_STATE, \
		 struct amdxdna_drm_set_state)

#if defined(__cplusplus)
} /* extern c end */
#endif

#endif /* AMDXDNA_ACCEL_H_ */
