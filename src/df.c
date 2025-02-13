/**
 * collectd - src/df.c
 * Copyright (C) 2005-2009  Florian octo Forster
 * Copyright (C) 2009       Paul Sadauskas
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 * Authors:
 *   Florian octo Forster <octo at collectd.org>
 *   Paul Sadauskas <psadauskas at gmail.com>
 **/

#include "collectd.h"

#include "plugin.h"
#include "utils/common/common.h"
#include "utils/ignorelist/ignorelist.h"
#include "utils/mount/mount.h"

#if HAVE_STATVFS
#if HAVE_SYS_STATVFS_H
#include <sys/statvfs.h>
#endif
#define STATANYFS statvfs
#define STATANYFS_STR "statvfs"
#define BLOCKSIZE(s) ((s).f_frsize ? (s).f_frsize : (s).f_bsize)
#elif HAVE_STATFS
#if HAVE_SYS_STATFS_H
#include <sys/statfs.h>
#endif
#define STATANYFS statfs
#define STATANYFS_STR "statfs"
#define BLOCKSIZE(s) (s).f_bsize
#else
#error "No applicable input method."
#endif

static const char *config_keys[] = {
    "Device",         "MountPoint",      "FSType",
    "IgnoreSelected", "ReportAvailUsed", "ReportByDevice",
    "ReportInodes",   "ValuesAbsolute",  "ValuesPercentage"};
static int config_keys_num = STATIC_ARRAY_SIZE(config_keys);

static ignorelist_t *il_device;
static ignorelist_t *il_mountpoint;
static ignorelist_t *il_fstype;

static bool by_device;
static bool report_availused = false;
static bool report_inodes;
static bool values_absolute = true;
static bool values_percentage;

static int df_init(void) {
  if (il_device == NULL)
    il_device = ignorelist_create(1);
  if (il_mountpoint == NULL)
    il_mountpoint = ignorelist_create(1);
  if (il_fstype == NULL)
    il_fstype = ignorelist_create(1);

  return 0;
}

static int df_config(const char *key, const char *value) {
  df_init();

  if (strcasecmp(key, "Device") == 0) {
    if (ignorelist_add(il_device, value))
      return 1;
    return 0;
  } else if (strcasecmp(key, "MountPoint") == 0) {
    if (ignorelist_add(il_mountpoint, value))
      return 1;
    return 0;
  } else if (strcasecmp(key, "FSType") == 0) {
    if (ignorelist_add(il_fstype, value))
      return 1;
    return 0;
  } else if (strcasecmp(key, "IgnoreSelected") == 0) {
    if (IS_TRUE(value)) {
      ignorelist_set_invert(il_device, 0);
      ignorelist_set_invert(il_mountpoint, 0);
      ignorelist_set_invert(il_fstype, 0);
    } else {
      ignorelist_set_invert(il_device, 1);
      ignorelist_set_invert(il_mountpoint, 1);
      ignorelist_set_invert(il_fstype, 1);
    }
    return 0;
  } else if (strcasecmp(key, "ReportAvailUsed") == 0) {
    if (IS_TRUE(value))
      report_availused = 1;
    else
      report_availused = 0;
    return (0);
  } else if (strcasecmp(key, "ReportByDevice") == 0) {
    if (IS_TRUE(value))
      by_device = true;

    return 0;
  } else if (strcasecmp(key, "ReportInodes") == 0) {
    if (IS_TRUE(value))
      report_inodes = true;
    else
      report_inodes = false;

    return 0;
  } else if (strcasecmp(key, "ValuesAbsolute") == 0) {
    if (IS_TRUE(value))
      values_absolute = true;
    else
      values_absolute = false;

    return 0;
  } else if (strcasecmp(key, "ValuesPercentage") == 0) {
    if (IS_TRUE(value))
      values_percentage = true;
    else
      values_percentage = false;

    return 0;
  }

  return -1;
}

__attribute__((nonnull(2))) static void df_submit_one(char *plugin_instance,
                                                      const char *type,
                                                      const char *type_instance,
                                                      gauge_t value) {
  value_list_t vl = VALUE_LIST_INIT;

  vl.values = &(value_t){.gauge = value};
  vl.values_len = 1;
  sstrncpy(vl.plugin, "df", sizeof(vl.plugin));
  if (plugin_instance != NULL)
    sstrncpy(vl.plugin_instance, plugin_instance, sizeof(vl.plugin_instance));
  sstrncpy(vl.type, type, sizeof(vl.type));
  if (type_instance != NULL)
    sstrncpy(vl.type_instance, type_instance, sizeof(vl.type_instance));

  plugin_dispatch_values(&vl);
} /* void df_submit_one */

static int df_read(void) {
#if HAVE_STATVFS
  struct statvfs statbuf;
#elif HAVE_STATFS
  struct statfs statbuf;
#endif
  int retval = 0;
  /* struct STATANYFS statbuf; */
  cu_mount_t *mnt_list;

  mnt_list = NULL;
  if (cu_mount_getlist(&mnt_list) == NULL) {
    ERROR("df plugin: cu_mount_getlist failed.");
    return -1;
  }

  for (cu_mount_t *mnt_ptr = mnt_list; mnt_ptr != NULL;
       mnt_ptr = mnt_ptr->next) {
    unsigned long long blocksize;
    char disk_name[256];
    cu_mount_t *dup_ptr;
    uint64_t blk_free;
    uint64_t blk_reserved;
    uint64_t blk_used;

    char const *dev =
        (mnt_ptr->spec_device != NULL) ? mnt_ptr->spec_device : mnt_ptr->device;

    if (ignorelist_match(il_device, dev))
      continue;
    if (ignorelist_match(il_mountpoint, mnt_ptr->dir))
      continue;
    if (ignorelist_match(il_fstype, mnt_ptr->type))
      continue;

    /* search for duplicates *in front of* the current mnt_ptr. */
    for (dup_ptr = mnt_list; dup_ptr != NULL; dup_ptr = dup_ptr->next) {
      /* No duplicate found: mnt_ptr is the first of its kind. */
      if (dup_ptr == mnt_ptr) {
        dup_ptr = NULL;
        break;
      }

      /* Duplicate found: leave non-NULL dup_ptr. */
      if (by_device && (mnt_ptr->spec_device != NULL) &&
          (dup_ptr->spec_device != NULL) &&
          (strcmp(mnt_ptr->spec_device, dup_ptr->spec_device) == 0))
        break;
      else if (!by_device && (strcmp(mnt_ptr->dir, dup_ptr->dir) == 0))
        break;
    }

    /* ignore duplicates */
    if (dup_ptr != NULL)
      continue;

    if (STATANYFS(mnt_ptr->dir, &statbuf) < 0) {
      ERROR(STATANYFS_STR "(%s) failed: %s", mnt_ptr->dir, STRERRNO);
      continue;
    }

    if (!statbuf.f_blocks)
      continue;

    if (by_device) {
      /* eg, /dev/hda1  -- strip off the "/dev/" */
      if (strncmp(dev, "/dev/", strlen("/dev/")) == 0)
        sstrncpy(disk_name, dev + strlen("/dev/"), sizeof(disk_name));
      else
        sstrncpy(disk_name, dev, sizeof(disk_name));

      if (strlen(disk_name) < 1) {
        DEBUG("df: no device name for mountpoint %s, skipping", mnt_ptr->dir);
        continue;
      }
    } else {
      if (strcmp(mnt_ptr->dir, "/") == 0)
        sstrncpy(disk_name, "root", sizeof(disk_name));
      else {
        sstrncpy(disk_name, mnt_ptr->dir + 1, sizeof(disk_name));
        size_t len = strlen(disk_name);

        for (size_t i = 0; i < len; i++)
          if (disk_name[i] == '/')
            disk_name[i] = '-';
      }
    }

    blocksize = BLOCKSIZE(statbuf);

/*
 * Sanity-check for the values in the struct
 */
/* Check for negative "available" byes. For example UFS can
 * report negative free space for user. Notice. blk_reserved
 * will start to diminish after this. */
#if HAVE_STATVFS
    /* Cast and temporary variable are needed to avoid
     * compiler warnings.
     * ((struct statvfs).f_bavail is unsigned (POSIX)) */
    int64_t signed_bavail = (int64_t)statbuf.f_bavail;
    if (signed_bavail < 0)
      statbuf.f_bavail = 0;
#elif HAVE_STATFS
    if (statbuf.f_bavail < 0)
      statbuf.f_bavail = 0;
#endif
    /* Make sure that f_blocks >= f_bfree >= f_bavail */
    if (statbuf.f_bfree < statbuf.f_bavail)
      statbuf.f_bfree = statbuf.f_bavail;
    if (statbuf.f_blocks < statbuf.f_bfree)
      statbuf.f_blocks = statbuf.f_bfree;

    blk_free = (uint64_t)statbuf.f_bavail;
    blk_reserved = (uint64_t)(statbuf.f_bfree - statbuf.f_bavail);
    blk_used = (uint64_t)(statbuf.f_blocks - statbuf.f_bfree);

    if (values_absolute) {
      df_submit_one(disk_name, "df_complex", "free",
                    (gauge_t)(blk_free * blocksize));
      df_submit_one(disk_name, "df_complex", "reserved",
                    (gauge_t)(blk_reserved * blocksize));
      df_submit_one(disk_name, "df_complex", "used",
                    (gauge_t)(blk_used * blocksize));
    }

    if (values_percentage) {
      if (statbuf.f_blocks > 0) {
        df_submit_one(disk_name, "percent_bytes", "free",
                      (gauge_t)((float_t)(blk_free) / statbuf.f_blocks * 100));
        df_submit_one(
            disk_name, "percent_bytes", "reserved",
            (gauge_t)((float_t)(blk_reserved) / statbuf.f_blocks * 100));
        df_submit_one(disk_name, "percent_bytes", "used",
                      (gauge_t)((float_t)(blk_used) / statbuf.f_blocks * 100));
        if (report_availused) {
          uint64_t available = blk_free + blk_used;

          df_submit_one(disk_name, "percent_avail", "availused",
                        (gauge_t)((float_t)(blk_used) / available * 100));
        }
      } else {
        retval = -1;
        break;
      }
    }

    /* inode handling */
    if (report_inodes && statbuf.f_files != 0 && statbuf.f_ffree != 0) {
      uint64_t inode_free;
      uint64_t inode_reserved;
      uint64_t inode_used;

      /* Sanity-check for the values in the struct */
      if (statbuf.f_ffree < statbuf.f_favail)
        statbuf.f_ffree = statbuf.f_favail;
      if (statbuf.f_files < statbuf.f_ffree)
        statbuf.f_files = statbuf.f_ffree;

      inode_free = (uint64_t)statbuf.f_favail;
      inode_reserved = (uint64_t)(statbuf.f_ffree - statbuf.f_favail);
      inode_used = (uint64_t)(statbuf.f_files - statbuf.f_ffree);

      if (values_percentage) {
        if (statbuf.f_files > 0) {
          df_submit_one(
              disk_name, "percent_inodes", "free",
              (gauge_t)((float_t)(inode_free) / statbuf.f_files * 100));
          df_submit_one(
              disk_name, "percent_inodes", "reserved",
              (gauge_t)((float_t)(inode_reserved) / statbuf.f_files * 100));
          df_submit_one(
              disk_name, "percent_inodes", "used",
              (gauge_t)((float_t)(inode_used) / statbuf.f_files * 100));
        } else {
          retval = -1;
          break;
        }
      }
      if (values_absolute) {
        df_submit_one(disk_name, "df_inodes", "free", (gauge_t)inode_free);
        df_submit_one(disk_name, "df_inodes", "reserved",
                      (gauge_t)inode_reserved);
        df_submit_one(disk_name, "df_inodes", "used", (gauge_t)inode_used);
      }
    }
  }

  cu_mount_freelist(mnt_list);

  return retval;
} /* int df_read */

void module_register(void) {
  plugin_register_config("df", df_config, config_keys, config_keys_num);
  plugin_register_init("df", df_init);
  plugin_register_read("df", df_read);
} /* void module_register */
