/*
 * Copyright (C) Tildeslash Ltd. All rights reserved.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * In addition, as a special exception, the copyright holders give
 * permission to link the code of portions of this program with the
 * OpenSSL library under certain conditions as described in each
 * individual source file, and distribute linked combinations
 * including the two.
 *
 * You must obey the GNU Affero General Public License in all respects
 * for all of the code used other than OpenSSL.
 */

/**
 *  System dependent filesystem methods.
 *
 *  @file
 */

#include "config.h"

#ifdef HAVE_STDIO_H
#include <stdio.h>
#endif

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_STRINGS_H
#include <strings.h>
#endif

#ifdef HAVE_SYS_STATVFS_H
# include <sys/statvfs.h>
#endif

#ifdef HAVE_SYS_TYPES_H
# include <sys/types.h>
#endif

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif

#ifdef HAVE_MNTENT_H
#include <mntent.h>
#endif

#ifdef HAVE_POLL_H
#include <poll.h>
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include "monit.h"

// libmonit
#include "io/File.h"
#include "system/Time.h"
#include "exceptions/AssertException.h"


/* ------------------------------------------------------------- Definitions */


#define MOUNTS   "/proc/self/mounts"
#define CIFSSTAT "/proc/fs/cifs/Stats"
#define DISKSTAT "/proc/diskstats"
#define NFSSTAT  "/proc/self/mountstats"


static struct {
        int fd;                                    // /proc/self/mounts filedescriptor (needed for mount/unmount notification)
        int generation;                            // Increment each time the mount table is changed
        boolean_t (*getBlockDiskActivity)(void *); // Disk activity callback: _getProcfsBlockDiskActivity (old kernels), _getSysfsBlockDiskActivity (new kernels)
        boolean_t (*getCifsDiskActivity)(void *);  // Disk activity callback: _getCifsDiskActivity if /proc/fs/cifs/Stats is present, otherwise _getDummyDiskActivity
} _statistics = {};


/* ----------------------------------------------------------------- Private */


static boolean_t _getDiskUsage(void *_inf) {
        Info_T inf = _inf;
        struct statvfs usage;
        if (statvfs(inf->filesystem->object.mountpoint, &usage) != 0) {
                LogError("Error getting usage statistics for filesystem '%s' -- %s\n", inf->filesystem->object.mountpoint, STRERROR);
                return false;
        }
        inf->filesystem->f_bsize = usage.f_frsize;
        inf->filesystem->f_blocks = usage.f_blocks;
        inf->filesystem->f_blocksfree = usage.f_bavail;
        inf->filesystem->f_blocksfreetotal = usage.f_bfree;
        inf->filesystem->f_files = usage.f_files;
        inf->filesystem->f_filesfree = usage.f_ffree;
        return true;
}


static boolean_t _getDummyDiskActivity(void *_inf) {
        return true;
}


static boolean_t _getCifsDiskActivity(void *_inf) {
        Info_T inf = _inf;
        FILE *f = fopen(CIFSSTAT, "r");
        if (! f) {
                LogError("Cannot open %s\n", CIFSSTAT);
                return false;
        }
        uint64_t now = Time_milli();
        char line[PATH_MAX];
        boolean_t found = false;
        while (fgets(line, sizeof(line), f)) {
                if (! found) {
                        int index;
                        char name[PATH_MAX];
                        if (sscanf(line, "%d) %1023s", &index, name) == 2 && IS(name, inf->filesystem->object.key)) {
                                found = true;
                        }
                } else if (found) {
                        char label1[256];
                        char label2[256];
                        uint64_t operations;
                        uint64_t bytes;
                        if (sscanf(line, "%255[^:]: %"PRIu64" %255[^:]: %"PRIu64, label1, &operations, label2, &bytes) == 4) {
                                if (IS(label1, "Reads") && IS(label2, "Bytes")) {
                                        Statistics_update(&(inf->filesystem->read.bytes), now, bytes);
                                        Statistics_update(&(inf->filesystem->read.operations), now, operations);
                                } else if (IS(label1, "Writes") && IS(label2, "Bytes")) {
                                        Statistics_update(&(inf->filesystem->write.bytes), now, bytes);
                                        Statistics_update(&(inf->filesystem->write.operations), now, operations);
                                        break;
                                }
                        }
                }
        }
        fclose(f);
        return true;
}


static boolean_t _getNfsDiskActivity(void *_inf) {
        Info_T inf = _inf;
        FILE *f = fopen(NFSSTAT, "r");
        if (! f) {
                LogError("Cannot open %s\n", NFSSTAT);
                return false;
        }
        uint64_t now = Time_milli();
        char line[PATH_MAX];
        char pattern[PATH_MAX];
        boolean_t found = false;
        snprintf(pattern, sizeof(pattern), "device %s ", inf->filesystem->object.device);
        while (fgets(line, sizeof(line), f)) {
                if (! found && Str_startsWith(line, pattern)) {
                        found = true;
                } else if (found) {
                        char name[256];
                        uint64_t operations;
                        uint64_t bytesSent;
                        uint64_t bytesReceived;
                        uint64_t time;
                        if (sscanf(line, " %255[^:]: %"PRIu64" %*u %*u %"PRIu64 " %"PRIu64" %*u %*u %"PRIu64, name, &operations, &bytesSent, &bytesReceived, &time) == 5) {
                                if (IS(name, "READ")) {
                                        Statistics_update(&(inf->filesystem->time.read), now, time / 1000.); // us -> ms
                                        Statistics_update(&(inf->filesystem->read.bytes), now, bytesReceived);
                                        Statistics_update(&(inf->filesystem->read.operations), now, operations);
                                } else if (IS(name, "WRITE")) {
                                        Statistics_update(&(inf->filesystem->time.write), now, time / 1000.); // us -> ms
                                        Statistics_update(&(inf->filesystem->write.bytes), now, bytesSent);
                                        Statistics_update(&(inf->filesystem->write.operations), now, operations);
                                        break;
                                }
                        }
                }
        }
        fclose(f);
        return true;
}


static boolean_t _getSysfsBlockDiskActivity(void *_inf) {
        Info_T inf = _inf;
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "/sys/class/block/%s/stat", inf->filesystem->object.key);
        FILE *f = fopen(path, "r");
        if (f) {
                uint64_t now = Time_milli();
                uint64_t readOperations = 0ULL, readSectors = 0ULL, readTime = 0ULL;
                uint64_t writeOperations = 0ULL, writeSectors = 0ULL, writeTime = 0ULL;
                if (fscanf(f, "%"PRIu64" %*u %"PRIu64" %"PRIu64" %"PRIu64" %*u %"PRIu64" %"PRIu64" %*u %*u %*u", &readOperations, &readSectors, &readTime, &writeOperations, &writeSectors, &writeTime) != 6) {
                        fclose(f);
                        LogError("filesystem statistic error: cannot parse %s -- %s\n", path, STRERROR);
                        return false;
                }
                Statistics_update(&(inf->filesystem->time.read), now, readTime);
                Statistics_update(&(inf->filesystem->read.bytes), now, readSectors * 512);
                Statistics_update(&(inf->filesystem->read.operations), now, readOperations);
                Statistics_update(&(inf->filesystem->time.write), now, writeTime);
                Statistics_update(&(inf->filesystem->write.bytes), now, writeSectors * 512);
                Statistics_update(&(inf->filesystem->write.operations), now, writeOperations);
                fclose(f);
                return true;
        }
        LogError("filesystem statistic error: cannot read %s -- %s\n", path, STRERROR);
        return false;
}


static boolean_t _getProcfsBlockDiskActivity(void *_inf) {
        Info_T inf = _inf;
        FILE *f = fopen(DISKSTAT, "r");
        if (f) {
                uint64_t now = Time_milli();
                uint64_t readOperations = 0ULL, readSectors = 0ULL;
                uint64_t writeOperations = 0ULL, writeSectors = 0ULL;
                char line[PATH_MAX];
                while (fgets(line, sizeof(line), f)) {
                        char name[256] = {};
                        // Fallback for kernels < 2.6.25: the /proc/diskstats used to have just 4 statistics, the file is present on >= 2.6.25 too and has 11 fields (same format as /sys/class/block/<NAME>/stat), but we use sysfs for it
                        // as we read the given partition directly instead of traversing the whole filesystems list. In this function we expect the old 4-statistics format - if it should be ever used as main data collector, it needs to
                        // be modified to support >= 2.6.25 format too.
                        if (fscanf(f, " %*d %*d %255s %"PRIu64" %"PRIu64" %"PRIu64" %"PRIu64, name, &readOperations, &readSectors, &writeOperations, &writeSectors) == 5 && IS(name, inf->filesystem->object.key)) {
                                Statistics_update(&(inf->filesystem->read.bytes), now, readSectors * 512);
                                Statistics_update(&(inf->filesystem->read.operations), now, readOperations);
                                Statistics_update(&(inf->filesystem->write.bytes), now, writeSectors * 512);
                                Statistics_update(&(inf->filesystem->write.operations), now, writeOperations);
                                fclose(f);
                                return true;
                        }
                }
        } else {
                LogError("filesystem statistic error: cannot read %s -- %s\n", DISKSTAT, STRERROR);
                return false;
        }
        return true;
}


static boolean_t _compareMountpoint(const char *mountpoint, struct mntent *mnt) {
        return IS(mountpoint, mnt->mnt_dir) && ! IS(mnt->mnt_fsname, "rootfs");
}


static boolean_t _compareDevice(const char *device, struct mntent *mnt) {
        char target[PATH_MAX] = {};
        // The device listed in /etc/mtab can be a device mapper symlink (e.g. /dev/mapper/centos-root -> /dev/dm-1) ... lookup the device as is first (support for NFS/CIFS/SSHFS/etc.) and fallback to realpath if it didn't match
        return (IS(device, mnt->mnt_fsname) || (realpath(mnt->mnt_fsname, target) && IS(device, target)));
}


static boolean_t _setDevice(Info_T inf, const char *path, boolean_t (*compare)(const char *path, struct mntent *mnt)) {
        FILE *f = setmntent(MOUNTS, "r");
        if (! f) {
                LogError("Cannot open %s\n", MOUNTS);
                return false;
        }
        inf->filesystem->object.generation = _statistics.generation;
        struct mntent *mnt;
        while ((mnt = getmntent(f))) {
                if (compare(path, mnt)) {
                        strncpy(inf->filesystem->object.device, mnt->mnt_fsname, sizeof(inf->filesystem->object.device) - 1);
                        strncpy(inf->filesystem->object.mountpoint, mnt->mnt_dir, sizeof(inf->filesystem->object.mountpoint) - 1);
                        strncpy(inf->filesystem->object.type, mnt->mnt_type, sizeof(inf->filesystem->object.type) - 1);
                        if (! IS(mnt->mnt_opts, inf->filesystem->flags)) {
                                if (*(inf->filesystem->flags)) {
                                        inf->filesystem->flagsChanged = true;
                                }
                                snprintf(inf->filesystem->flags, sizeof(inf->filesystem->flags), "%s", mnt->mnt_opts);
                        }
                        inf->filesystem->object.getDiskUsage = _getDiskUsage; // The disk usage method is common for all filesystem types
                        inf->filesystem->object.getDiskActivity = _getDummyDiskActivity; // Set to dummy IO statistics method by default (can be overriden bellow if statistics method is available for this filesystem)
                        if (Str_startsWith(mnt->mnt_type, "nfs")) {
                                // NFS
                                inf->filesystem->object.getDiskActivity = _getNfsDiskActivity;
                        } else if (IS(mnt->mnt_type, "cifs")) {
                                // CIFS
                                inf->filesystem->object.getDiskActivity = _statistics.getCifsDiskActivity;
                                // Need Windows style name - replace '/' with '\' so we can lookup the filesystem activity in /proc/fs/cifs/Stats
                                strncpy(inf->filesystem->object.key, inf->filesystem->object.device, sizeof(inf->filesystem->object.key) - 1);
                                Str_replaceChar(inf->filesystem->object.key, '/', '\\');
                        } else {
                                if (realpath(mnt->mnt_fsname, inf->filesystem->object.key)) {
                                        // Need base name for /sys/class/block/<NAME>/stat or /proc/diskstats lookup:
                                        snprintf(inf->filesystem->object.key, sizeof(inf->filesystem->object.key), "%s", File_basename(inf->filesystem->object.key));
                                        // Test if block device statistics are available for the given filesystem
                                        if (_statistics.getBlockDiskActivity(inf)) {
                                                // Block device
                                                inf->filesystem->object.getDiskActivity = _statistics.getBlockDiskActivity;
                                        }
                                }
                        }
                        endmntent(f);
                        inf->filesystem->object.mounted = true;
                        return true;
                }
        }
        LogError("Lookup for '%s' filesystem failed  -- not found in %s\n", path, MOUNTS);
error:
        endmntent(f);
        inf->filesystem->object.mounted = false;
        return false;
}


static boolean_t _getDevice(Info_T inf, const char *path, boolean_t (*compare)(const char *path, struct mntent *mnt)) {
        // Mount/unmount notification: open the /proc/self/mounts file if we're in daemon mode and keep it open until monit
        // stops, so we can poll for mount table changes
        // FIXME: when libev is added register the mount table handler in libev and stop polling here
        if (_statistics.fd == -1 && (Run.flags & Run_Daemon) && ! (Run.flags & Run_Once)) {
                _statistics.fd = open(MOUNTS, O_RDONLY);
        }
        if (_statistics.fd != -1) {
                struct pollfd mountNotify = {.fd = _statistics.fd, .events = POLLPRI, .revents = 0};
                if (poll(&mountNotify, 1, 0) != -1) {
                        if (mountNotify.revents & POLLERR) {
                                DEBUG("Mount table change detected\n");
                                _statistics.generation++;
                        }
                } else {
                        LogError("Mount table polling failed -- %s\n", STRERROR);
                }
        }
        if (inf->filesystem->object.generation != _statistics.generation || _statistics.fd == -1) {
                DEBUG("Reloading mount informations for filesystem '%s'\n", path);
                _setDevice(inf, path, compare);
        }
        if (inf->filesystem->object.mounted) {
                return (inf->filesystem->object.getDiskUsage(inf) && inf->filesystem->object.getDiskActivity(inf));
        }
        return false;
}


/* --------------------------------------- Static constructor and destructor */


static void __attribute__ ((constructor)) _constructor() {
        struct stat sb;
        _statistics.fd = -1;
        _statistics.generation++; // First generation
        _statistics.getBlockDiskActivity = stat("/sys/class/block", &sb) == 0 ? _getSysfsBlockDiskActivity : _getProcfsBlockDiskActivity;
        _statistics.getCifsDiskActivity = stat(CIFSSTAT, &sb) == 0 ? _getCifsDiskActivity : _getDummyDiskActivity;
}


static void __attribute__ ((destructor)) _destructor() {
        if (_statistics.fd > -1) {
                  close(_statistics.fd);
        }
}


/* ------------------------------------------------------------------ Public */


boolean_t Filesystem_getByMountpoint(Info_T inf, const char *path) {
        ASSERT(inf);
        ASSERT(path);
        return _getDevice(inf, path, _compareMountpoint);
}


boolean_t Filesystem_getByDevice(Info_T inf, const char *path) {
        ASSERT(inf);
        ASSERT(path);
        return _getDevice(inf, path, _compareDevice);
}

