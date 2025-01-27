 /**
 * Copyright (C) <2012> <Syracuse System Security (Sycure) Lab>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the Free
 * Software Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 **/
 
 /*
 * DECAF_main.c
 *
 *	Created on: Oct 14, 2012
 *		Author: lok
 */
 
#include <dlfcn.h>
#include "sysemu/sysemu.h"
#include "block/block_int.h"
#include "exec/ram_addr.h"
#include "tcg/tcg.h"
 
 
#include "DECAF_shared/DECAF_config.h"
#include "DECAF_shared/linux_vmi_.h"
#include "DECAF_shared/dalvik_vmi.h"
#include "DECAF_shared/DECAF_main.h"
#include "DECAF_shared/DECAF_main_internal.h"
#include "DECAF_shared/DECAF_vm_compress.h"
#include "DECAF_shared/DECAF_cmds.h"
#include "DECAF_shared/DECAF_fileio.h"
#include "DECAF_shared/art_vmi.h"
 
 extern void VMI_init(void);
 
 
 //#include "DECAF_shared/procmod.h" //remove this later
 disk_info_t disk_info_internal[8];
 
 int should_monitor = 1;
 int devices_ = 0;
 static int total_devices = 0;
 
 
 plugin_interface_t *decaf_plugin = NULL;
 static void *plugin_handle = NULL;
 static char decaf_plugin_path[PATH_MAX] = "";
 static FILE *decaflog = NULL;
 
#include "DECAF_shared/DECAF_mon_cmds_defs.h"
 
 mon_cmd_t DECAF_mon_cmds[] = {
    #include "DECAF_mon_cmds.h"
	 {NULL, NULL, },
 };
 
 mon_cmd_t DECAF_info_cmds[] = {
    #include "DECAF_info_cmds.h"
	 {NULL, NULL, },
 };

 gpa_t DECAF_get_phys_addr(CPUArchState* env, gva_t addr)
 {
	 int mmu_idx, index;
	 gpa_t phys_addr;
	 
	 if (env == NULL)
	 {
		 return(INV_ADDR);
	 }
	 
	 index = (addr >> TARGET_PAGE_BITS) & (CPU_TLB_SIZE - 1);
	 mmu_idx = cpu_mmu_index(env);
	 if (__builtin_expect(env->tlb_table[mmu_idx][index].addr_read !=
	 (addr & TARGET_PAGE_MASK), 0)) {
		 if(__builtin_expect(env->tlb_table[mmu_idx][index].addr_code !=
		 (addr & TARGET_PAGE_MASK), 0)) {
			 phys_addr = cpu_get_phys_page_debug(env, addr & TARGET_PAGE_MASK);
			 if (phys_addr == -1)
				 return -1;
			 phys_addr += addr & (TARGET_PAGE_SIZE - 1);
			 return phys_addr;
		 }
	 }
    #if 0                           //not sure if we need it --Heng Yin
		 pd = env->tlb_table[mmu_idx][index].addr_read & ~TARGET_PAGE_MASK;
	 if (pd > IO_MEM_ROM && !(pd & IO_MEM_ROMD)) {
		 cpu_abort(env,
		 "Trying to execute code outside RAM or ROM at 0x"
		 TARGET_FMT_lx "\n", addr);
	 }
    #endif
	 return (gpa_t) qemu_ram_addr_from_host_nofail (
	 (void*)((addr & TARGET_PAGE_MASK) +
	 env->tlb_table[mmu_idx][index].addend));
 }
 
 DECAF_errno_t DECAF_read_mem__(CPUArchState* env, target_ulong virt_addr,
							target_ulong size, void *buffer, target_ulong buffer_size)
{

	uint32_t l;
	CPUState *cpu = mon_get_cpu_external(NULL); //ENV_GET_CPU(env);//
	
	//CPUArchState *cpu = mon_get_cpu_external(NULL);
    
    //uint8_t buf[1024];
    int64_t orig_addr = virt_addr, orig_size = size;

    while (size != 0) {
        l = buffer_size;
        if (l > size)
            l = size;
        if (cpu_memory_rw_debug(cpu, virt_addr, buffer, l, 0) != 0) {
            //monitor_printf(default_mon, "Invalid addr 0x%016" PRIx64 "/size %" PRId64
            //                " specified", orig_addr, orig_size);
            return -1;
        }
        virt_addr += l;
        size -= l;
    }
  
  return 0;
}
 
 DECAF_errno_t DECAF_memory_rw(CPUArchState* env, gva_t addr, void *buf, int len, int is_write)
 {
	 int l;
	 gpa_t page, phys_addr;
	 
	 if (env == NULL)
	 {
		 return(INV_ADDR);
	 }
	 
	 while (len > 0) {
	 	
		 page = addr & TARGET_PAGE_MASK;
		 phys_addr = DECAF_get_phys_addr(env, page);

		 
		 if (phys_addr == -1 || phys_addr > ram_size) {
			 return -1;
		 }
		 l = (page + TARGET_PAGE_SIZE) - addr;
		 if (l > len)
			 l = len;
		 
		 cpu_physical_memory_rw(phys_addr + (addr & ~TARGET_PAGE_MASK),
		 buf, l, is_write);
		 
		 len -= l;
		 buf += l;
		 addr += l;
	 }
	 return 0;
 }
 
 DECAF_errno_t DECAF_memory_rw_with_pgd(CPUArchState* env, gpa_t pgd, gva_t addr, void *buf, int len, int is_write)
 {
	 if (env == NULL)
	 {
		 return (INV_ADDR);
	 }
	 
	 int l;
	 gpa_t page, phys_addr;
	 
	 while (len > 0) {
		 page = addr & TARGET_PAGE_MASK;
		 phys_addr = DECAF_get_phys_addr_with_pgd(env, pgd, page);
		 if (phys_addr == -1)
			 return -1;
		 l = (page + TARGET_PAGE_SIZE) - addr;
		 if (l > len)
			 l = len;
		 cpu_physical_memory_rw(phys_addr + (addr & ~TARGET_PAGE_MASK),
		 buf, l, is_write);
		 len -= l;
		 buf += l;
		 addr += l;
	 }
	 return 0;
 }
 
 int DECAF_read_mem_until(CPUArchState* env, gva_t vaddr, void* buf, size_t len)
 {
	 int i = 0;
	 int ret = 0;
	 
	 if (buf == NULL)
	 {
		 return (NULL_POINTER_ERROR);
	 }
	 
	 for (i = 0; i < len; i++)
	 {
		 ret = DECAF_read_mem(env, vaddr + i, buf + i, 1);
		 if (ret != 0)
		 {
			 break;
		 }
	 }
	 return (i);
 }
 
 //copied from cpu-exec.c
 static TranslationBlock *DECAF_tb_find_slow(CPUArchState *env,
 gva_t pc,
 gva_t cs_base,
 uint64_t flags)
 {
	 TranslationBlock *tb, **ptb1;
	 unsigned int h;
	 target_ulong phys_pc, phys_page1, phys_page2, virt_page2;
	 
	 tcg_ctx.tb_ctx.tb_invalidated_flag = 0;
	 
	 /* find translated block using physical mappings */
	 phys_pc = get_page_addr_code(env, pc);
	 
	 if (phys_pc == -1) //they use -1 (INV_ADDR) as well.
	 {
		 return (NULL);
	 }
	 
	 
	 phys_page1 = phys_pc & TARGET_PAGE_MASK;
	 phys_page2 = -1;
	 h = tb_phys_hash_func(phys_pc);
	 ptb1 = &tcg_ctx.tb_ctx.tb_phys_hash[h];
	 for(;;) {
		 tb = *ptb1;
		 if (!tb)
			 goto not_found;
		 if (tb->pc == pc &&
			 tb->page_addr[0] == phys_page1 &&
		 tb->cs_base == cs_base &&
		 tb->flags == flags) {
			 /* check next page if needed */
			 if (tb->page_addr[1] != -1) {
				 virt_page2 = (pc & TARGET_PAGE_MASK) +
				 TARGET_PAGE_SIZE;
				 phys_page2 = get_page_addr_code(env, virt_page2);
				 if (tb->page_addr[1] == phys_page2)
					 goto found;
			 } else {
				 goto found;
			 }
		 }
		 ptb1 = &tb->phys_hash_next;
	 }
	 not_found:
	 //Removed
	 return (NULL);
	 
	 found:
	 //Removed
	 return tb;
 }
 
 // This is the same as tb_find_fast except we invalidate at the end
 void DECAF_flushTranslationBlock_env(CPUArchState *env, gva_t addr)
 {
	 TranslationBlock *tb;
	 gva_t cs_base, pc;
	 int flags;
	 
	 if (env == NULL)
	 {
		 return;
	 }
	 
	 /* we record a subset of the CPU state. It will
	 always be the same before a given translated block
	 is executed. */
	 cpu_get_tb_cpu_state(env, &pc, &cs_base, &flags);
	 tb = env->tb_jmp_cache[tb_jmp_cache_hash_func(pc)];
	 if (unlikely(!tb || tb->pc != pc || tb->cs_base != cs_base ||
	 tb->flags != flags)) {
		 tb = DECAF_tb_find_slow(env, pc, cs_base, flags);
	 }
	 if (tb == NULL)
	 {
		 return;
	 }
	 
	 //this is what we added
	 tb_phys_invalidate(tb, -1);
 }
 
 void DECAF_flushTranslationPage_env(CPUArchState* env, gva_t addr)
 {
	 tb_page_addr_t p_addr;
	 
	 if (env == NULL)
	 {
		 return;
	 }
	 
	 p_addr = cpu_get_phys_page_debug(env, addr);
	 if (p_addr != -1)
	 {
		 p_addr &= TARGET_PAGE_MASK;
		 tb_invalidate_phys_page_range(p_addr, p_addr + TARGET_PAGE_SIZE, 0); //not sure if this will work, but might as well try it
	 }
 }
 
 int do_load_plugin(Monitor *mon, const QDict *qdict, QObject **ret_data)
 {
	 DECAF_do_load_plugin_internal(mon, qdict_get_str(qdict, "filename"));
	 return (0);
 }
 
 void DECAF_do_load_plugin_internal(Monitor *mon, const char *plugin_path)
 {
	 plugin_interface_t *(*init_plugin) (void);
	 char *error;
	 
	 if (decaf_plugin_path[0]) {
		 monitor_printf(mon, "%s has already been loaded! \n", plugin_path);
		 return;
	 }
	 
	 plugin_handle = dlopen(plugin_path, RTLD_NOW);
	 if (NULL == plugin_handle) {
		 // AWH
		 char tempbuf[128];
		 strncpy(tempbuf, dlerror(), 127);
		 monitor_printf(mon, "%s\n", tempbuf);
		 fprintf(stderr, "%s COULD NOT BE LOADED - ERR = [%s]\n",
		 plugin_path, tempbuf);
		 //assert(0);
		 return;
	 }
	 
	 dlerror();
	 
	 init_plugin = dlsym(plugin_handle, "init_plugin");
	 if ((error = dlerror()) != NULL) {
		 fprintf(stderr, "%s\n", error);
		 dlclose(plugin_handle);
		 plugin_handle = NULL;
		 return;
	 }
	 
	 decaf_plugin = init_plugin();
	 
	 if (NULL == decaf_plugin) {
		 monitor_printf(mon, "fail to initialize the plugin!\n");
		 dlclose(plugin_handle);
		 plugin_handle = NULL;
		 return;
	 }
	 
	 decaflog = fopen("decaf.log", "w");
	 assert(decaflog != NULL);
	 
	 if (decaf_plugin->bdrv_open)
	 {
		 BlockInterfaceType interType = IF_NONE;
		 int index = 0;
		 DriveInfo *drvInfo = NULL;
		 for (interType = IF_NONE; interType < IF_COUNT; interType++)
		 {
			 index = 0;
			 do
			 {
				 //LOK: Older qemu versions don't have this function
				 // so we just inline the new definition - it
				 // gets pretty involved
                #ifndef QEMU_ANDROID_GINGERBREAD
				 drvInfo = drive_get_by_index(interType, index);
                #else
					 int _bus_id = 0;
				 int _unit_id = 0;
				 int _max_devs = 0;
				 //static const int if_max_devs[IF_COUNT] = {
				 // [IF_IDE] = 2,
				 // [IF_SCSI] = 7,
				 //}
				 if (interType == IF_IDE)
				 {
					 _max_devs = 2;
				 }
				 else if (interType == IF_SCSI)
				 {
					 _max_devs = 7;
				 }
				 //static int drive_index_to_bus_id(BlockInterfaceType type, int index)
				 //{
				 //    int max_devs = if_max_devs[type];
				 //    return max_devs ? index / max_devs : 0;
				 //}
				 _bus_id = _max_devs ? index / _max_devs : 0;
				 //static int drive_index_to_unit_id(BlockInterfaceType type, int index)
				 //{
				 //    int max_devs = if_max_devs[type];
				 //    return max_devs ? index % max_devs : index;
				 //}
				 _unit_id = _max_devs ? index % _max_devs : index;
				 
				 drvInfo = drive_get(interType,
				 //drive_index_to_bus_id(interType, index),
				 _bus_id,
				 //drive_index_to_uint_id(interType, index));
				 _unit_id);
                #endif
				 if (drvInfo && drvInfo->bdrv)
					 decaf_plugin->bdrv_open(interType, index, drvInfo->bdrv);
				 index++;
			 } while (drvInfo);
		 }
	 }
	 
	 strncpy(decaf_plugin_path, plugin_path, PATH_MAX);
	 monitor_printf(mon, "%s is loaded successfully!\n", plugin_path);
 }
 
 int do_unload_plugin(Monitor *mon, const QDict *qdict, QObject **ret_data)
 {
	 if (decaf_plugin_path[0]) {
		 decaf_plugin->plugin_cleanup();
		 fclose(decaflog);
		 decaflog = NULL;
		 
		 //Flush all the callbacks that the plugin might have registered for
		 //hookapi_flush_hooks(decaf_plugin_path);
		 // NOT NEEDED HERE!! cleanup_insn_cbs();
		 //LOK: Created a new callback interface for procmod
		 // 	   loadmainmodule_notify = createproc_notify = removeproc_notify = loadmodule_notify = NULL;
		 
		 dlclose(plugin_handle);
		 plugin_handle = NULL;
		 decaf_plugin = NULL;
		 
        #if 0 //LOK: Removed // AWH TAINT_ENABLED
			 taintcheck_cleanup();
        #endif
		 monitor_printf(default_mon, "%s is unloaded!\n", decaf_plugin_path);
		 decaf_plugin_path[0] = 0;

		 //clear_oat_dumpers();
	 }
	 else
	 {
		 monitor_printf(default_mon, "Can't unload plugin because no plugin was loaded!\n");
	 }
	 
	 return (0);
 }
 
 
 void DECAF_stop_vm(void)
 {
	 /*  CPUState *env = cpu_single_env? cpu_single_env: mon_get_cpu();
	 env->exception_index = EXCP_HLT;
	 longjmp(env->jmp_env, 1); */
    #ifdef QEMU_ANDROID_GINGERBREAD
	 //LOK: In the QEMU version in gingerbread, we can't use RUN_STATE_PAUSED - not
	 // introduced yet
	 vm_stop(EXCP_INTERRUPT);
    #else
		 vm_stop(RUN_STATE_PAUSED);
    #endif
 }
 
 void DECAF_start_vm(void)
 {
	 vm_start();
 }
 
 void DECAF_loadvm(void *opaque)
 {
	 char **loadvm_args = opaque;
	 if (loadvm_args[0]) {
        #ifdef QEMU_ANDROID_GINGERBREAD
		 do_loadvm(default_mon, loadvm_args[0]);
        #else
			 load_vmstate(loadvm_args[0]);
        #endif
		 free(loadvm_args[0]);
		 loadvm_args[0] = NULL;
	 }
	 
	 if (loadvm_args[1]) {
		 DECAF_do_load_plugin_internal(default_mon, loadvm_args[1]);
		 free(loadvm_args[1]);
		 loadvm_args[1] = NULL;
	 }
	 
	 if (loadvm_args[2]) {
		 DECAF_after_loadvm(loadvm_args[2]);
		 free(loadvm_args[2]);
		 loadvm_args[2] = NULL;
	 }
 }
 
 static FILE *guestlog = NULL;
 
 static void DECAF_save(QEMUFile * f, void *opaque)
 {
	 size_t len = strlen(decaf_plugin_path) + 1;
	 qemu_put_be32(f, len);
	 qemu_put_buffer(f, (const uint8_t *)decaf_plugin_path, len); // AWH - cast
	 
	 //save guest.log
	 //we only save guest.log when no plugin is loaded
	 if (len == 1) {
		 FILE *fp = fopen("guest.log", "r");
		 size_t size;
		 if (!fp) {
			 fprintf(stderr, "cannot open guest.log!\n");
			 return;
		 }
		 
		 fseek(fp, 0, SEEK_END);
		 size = ftell(fp);
		 qemu_put_be32(f, size);
		 rewind(fp);
		 if (size > 0) {
			 DECAF_CompressState_t state;
			 if (DECAF_compress_open(&state, f) < 0)
				 return;
			 
			 while (!feof(fp)) {
				 uint8_t buf[4096];
				 size_t res = fread(buf, 1, sizeof(buf), fp);
				 DECAF_compress_buf(&state, buf, res);
			 }
			 
			 DECAF_compress_close(&state);
		 }
		 fclose(fp);
	 }
	 
	 qemu_put_be32(f, 0x12345678);		 //terminator
 }
 
 static int DECAF_load(QEMUFile * f, void *opaque, int version_id)
 {
	 size_t len = qemu_get_be32(f);
	 char tmp_plugin_path[PATH_MAX];
	 
	 if (plugin_handle)
	 {
		 do_unload_plugin(NULL, NULL, NULL); // AWH - Added NULLs
	 }
	 qemu_get_buffer(f, (uint8_t *)tmp_plugin_path, len); // AWH - cast
	 if (tmp_plugin_path[len - 1] != 0)
		 return -EINVAL;
	 
	 //load guest.log
	 if (len == 1) {
		 fclose(guestlog);
		 if (!(guestlog = fopen("guest.log", "w"))) {
			 fprintf(stderr, "cannot open guest.log for write!\n");
				 return -EINVAL;
		 }
		 
		 size_t file_size = qemu_get_be32(f);
		 uint8_t buf[4096];
		 size_t i;
		 DECAF_CompressState_t state;
		 if (DECAF_decompress_open(&state, f) < 0)
			 return -EINVAL;
		 
		 for (i = 0; i < file_size;) {
			 size_t len =
			 (sizeof(buf) <
			 file_size - i) ? sizeof(buf) : file_size - i;
			 if (DECAF_decompress_buf(&state, buf, len) < 0)
				 return -EINVAL;
			 
			 fwrite(buf, 1, len, guestlog);
			 i += len;
		 }
		 DECAF_decompress_close(&state);
		 fflush(guestlog);
	 }
	 
	 if (len > 1)
		 DECAF_do_load_plugin_internal(default_mon, tmp_plugin_path);
	 
	 uint32_t terminator = qemu_get_be32(f);
	 if (terminator != 0x12345678)
		 return -EINVAL;
	 
	 return 0;
 }
 
 
 extern void init_hookapi(void);
 extern void function_map_init(void);
 extern void DECAF_callback_init(void);
 
 
 void DECAF_init(void)
 {
	 DECAF_callback_init();
	 VMI_init();
	 art_vmi_init();
	 //dalvik_vmi_init();
	 //DECAF_virtdev_init();
	 
	 // AWH - change in API, added NULL as first parm
	 /* Aravind - NOTE: TEMU_save *must* be called before function_map_save and TEMU_load must be called
	 * before function_map_load for function maps to load properly during loadvm.
	 * This is because, TEMU_load restores guest.log, which is read into function map.
	 */
	 // REGISTER_SAVEVM(NULL, "TEMU", 0, 1, DECAF_save, DECAF_load, NULL);
	 
	 
	 DECAF_vm_compress_init();
	 
	 //AVB - We will deal with this a little later
	 //DS_init();
 }
 
 
 /*
 * NIC related functions
 */
 
 void DECAF_nic_receive(const uint8_t * buf, int size, int cur_pos,
 int start, int stop)
 {
	 if (decaf_plugin && decaf_plugin->nic_recv)
		 decaf_plugin->nic_recv((uint8_t *) buf, size, cur_pos, start, stop);
 }
 
 
 void DECAF_nic_send(gva_t addr, int size, uint8_t * buf)
 {
	 if (decaf_plugin && decaf_plugin->nic_send)
		 decaf_plugin->nic_send(addr, size, buf);
 }
 
 
 void DECAF_nic_out(gva_t addr, int size)
 {
	 if (!DECAF_emulation_started)
		 return;
 }
 
 
 void DECAF_nic_in(gva_t addr, int size)
 {
	 if (!DECAF_emulation_started)
		 return;
 }
 void DECAF_after_loadvm(const char *param)
 {
	 if (decaf_plugin && decaf_plugin->after_loadvm)
		 decaf_plugin->after_loadvm(param);
 }
 
 DECAF_errno_t DECAF_read_ptr(CPUArchState* env, gva_t vaddr, gva_t *pptr)
 {
	 int ret = DECAF_read_mem(env, vaddr, pptr, sizeof(gva_t) );
	 if(0 == ret)
	 {
        #ifdef TARGET_WORDS_BIGENDIAN
		 convert_endian_4b(pptr);
        #endif
	 }
	 return ret;
 }
 
 // AVB, This function is used to read 'n' bytes off the disk images give by `opaque'
 // at an offset
 int DECAF_bdrv_pread(void *opaque, int64_t offset, void *buf, int count) {
	 
	 return bdrv_pread((BlockDriverState *)opaque, offset, buf, count);
	 
 }

 static assign_device_number(char *file_name) {
	if(strstr(file_name, "system") != NULL)
		devices_ = SYSTEM_PARTITION;
	else if(strstr(file_name, "userdata") != NULL)
		devices_ = DATA_PARTITION;
	else
		devices_ = CACHE_PARTITION;
 }
 	
 // AVB, This function is used to open the disk on sleuthkit by calling `tsk_fs_open_img'.
 void DECAF_bdrv_open(char *image_path, uint64_t image_size, bool by_filename, void *opaque) {

	 unsigned long img_size;// = ((BlockDriverState *)opaque)->total_sectors * 512;
	 int ret = 0;
	 
	 char *drive_path = NULL;// = ((BlockDriverState *)opaque)->filename;
	 
	 if(!qemu_pread)
	 	qemu_pread=(qemu_pread_t)DECAF_bdrv_pread;
	 
	 if(by_filename)
	 {
		 drive_path = image_path;
		 assign_device_number(drive_path);
	 	 disk_info_internal[devices_].img = tsk_img_open(1, (const char **) &image_path, TSK_IMG_TYPE_DETECT, 0);
	 }
	 else
	 {
	 	 printf("without file_path path - %s\n", drive_path);
	 	 img_size = ((BlockDriverState *)opaque)->total_sectors * 512;
		 drive_path = ((BlockDriverState *)opaque)->filename;
 		 assign_device_number(drive_path);
		 disk_info_internal[devices_].bs = opaque;
		 disk_info_internal[devices_].img = tsk_img_open(1, (const char **) &opaque, QEMU_IMG, 0);
	 }
	 
	 disk_info_internal[devices_].img->size = img_size;
	 
	 if (disk_info_internal[devices_].img == NULL)
	 {	
 		 disk_info_internal[devices_].fs = NULL;
		 printf("FAILED! device number - %d path - %s\n", devices_, (drive_path == NULL) ?  "other " : drive_path);
		 goto out;
 	 }

	 
	 
	 // TODO: AVB, also add an option of 56 as offset with sector size of 4k, Sector size is now assumed to be 512 by default
	 //if(!(disk_info_internal[devices_].fs = tsk_fs_open_img(disk_info_internal[devices_].img, 0 ,TSK_FS_TYPE_EXT_DETECT)))

	 if(!(disk_info_internal[devices_].fs = tsk_fs_open_img(disk_info_internal[devices_].img, 0 ,TSK_FS_TYPE_EXT4)) &&
	 	!(disk_info_internal[devices_].fs = tsk_fs_open_img(disk_info_internal[devices_].img, 0 ,TSK_FS_TYPE_EXT_DETECT)) &&
	 	!(disk_info_internal[devices_].fs = tsk_fs_open_img(disk_info_internal[devices_].img, 63 * (disk_info_internal[devices_].img)->sector_size, TSK_FS_TYPE_EXT_DETECT)) &&
		 !(disk_info_internal[devices_].fs = tsk_fs_open_img(disk_info_internal[devices_].img, 2048 * (disk_info_internal[devices_].img)->sector_size , TSK_FS_TYPE_EXT_DETECT)) )
	 
	 {	
		 printf("FAILED! device number - %d path - %s\n", devices_, (drive_path == NULL) ?  "other " : drive_path);
		 disk_info_internal[devices_].fs = NULL;
		 goto out;
	 }
	 else
	 {
	 	 ++total_devices;
		 printf("OPEN! device number - %d path - %s\n", devices_, (drive_path == NULL) ?  "other " : drive_path);
		 goto out;
	 }

	 out:
	 	 ++devices_;
	 
 }
 
 
 
 
