/*
   Copyright (c) 2017, 2019, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include <my_global.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#if defined(HAVE_GETMNTENT) || defined(HAVE_GETMNTENT_R)
#include <mntent.h>
#elif !defined(HAVE_GETMNTINFO_TAKES_statvfs)
/* getmntinfo (the not NetBSD variants) */
#include <sys/param.h>
#include <sys/ucred.h>
#include <sys/mount.h>
#endif
#include <sql_class.h>
#include <sql_i_s.h>
#include <sql_acl.h>                            /* check_global_access() */

bool schema_table_store_record(THD *thd, TABLE *table);


struct st_mysql_information_schema disks_table_info = { MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION };


namespace Show {

ST_FIELD_INFO disks_table_fields[]=
{
  Column("Disk",      Varchar(PATH_MAX), NOT_NULL),
  Column("Path",      Varchar(PATH_MAX), NOT_NULL),
  Column("Total",     SLonglong(32),     NOT_NULL), // Total amount available
  Column("Used",      SLonglong(32),     NOT_NULL), // Amount of space used
  Column("Available", SLonglong(32),     NOT_NULL), // Amount available to users other than root.
  CEnd()
};

#if defined(HAVE_GETMNTINFO_TAKES_statvfs) || defined(HAVE_GETMNTENT)
static int disks_table_add_row_statvfs(
#else
static int disks_table_add_row_statfs(
#endif
                                      THD* pThd,
                                      TABLE* pTable,
                                      const char* zDisk,
                                      const char* zPath,
#if defined(HAVE_GETMNTINFO_TAKES_statvfs) || defined(HAVE_GETMNTENT)
                                      const struct statvfs& info
#elif defined(HAVE_GETMNTINFO64)
                                      const struct statfs64& info
#else // GETMNTINFO
                                      const struct statfs& info
#endif
				      )
{
    // From: http://pubs.opengroup.org/onlinepubs/009695399/basedefs/sys/statvfs.h.html
    // and same for statfs:
    // From: https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man2/statfs.2.html#//apple_ref/doc/man/2/statfs
    // and: https://www.freebsd.org/cgi/man.cgi?query=statfs&sektion=2&apropos=0&manpath=FreeBSD+13.1-RELEASE+and+Ports
    //
    // f_bsize    Fundamental file system block size.
    // f_blocks   Total number of blocks on file system in units of f_frsize.
    // f_bfree    Total number of free blocks.
    // f_bavail   Number of free blocks available to non-privileged process.
    ulong block_size= (ulong) info.f_bsize;

    ulonglong total = ((ulonglong) block_size * info.f_blocks) / 1024;
    ulonglong used  = ((ulonglong) block_size *
                            (info.f_blocks - info.f_bfree)) / 1024;
    ulonglong avail = ((ulonglong) block_size * info.f_bavail) / 1024;

    pTable->field[0]->store(zDisk, strlen(zDisk), system_charset_info);
    pTable->field[1]->store(zPath, strlen(zPath), system_charset_info);
    pTable->field[2]->store(total);
    pTable->field[3]->store(used);
    pTable->field[4]->store(avail);

    // 0 means success.
    return (schema_table_store_record(pThd, pTable) != 0) ? 1 : 0;
}


#ifdef HAVE_GETMNTENT
int disks_table_add_row(THD* pThd, TABLE* pTable, const char* zDisk, const char* zPath)
{
    int rv = 0;

    struct statvfs info;

    if (statvfs(zPath, &info) == 0) // We ignore failures.
    {
        rv = disks_table_add_row_statvfs(pThd, pTable, zDisk, zPath, info);
    }

    return rv;
}
#endif


#if defined(HAVE_GETMNTINFO)
int disks_fill_table(THD* pThd, TABLE_LIST* pTables, Item* pCond)
{
#if defined(HAVE_GETMNTINFO_TAKES_statvfs)
    struct statvfs *s;
#elif defined(HAVE_GETMNTINFO64)
    struct statfs64 *s;
#else
    struct statfs *s;
#endif
    int count, rv= 0;
    TABLE* pTable = pTables->table;

    if (check_global_access(pThd, FILE_ACL, true))
      return 0;

#if defined(HAVE_GETMNTINFO_TAKES_statvfs)
    count= getmntinfo(&s, ST_WAIT);
#elif defined(HAVE_GETMNTINFO64)
    count= getmntinfo64(&s, MNT_WAIT);
#else
    count= getmntinfo(&s, MNT_WAIT);
#endif
    if (count == 0)
        return 1;

    while (count && rv == 0)
    {
#if defined(HAVE_GETMNTINFO_TAKES_statvfs)
        rv = disks_table_add_row_statvfs(pThd, pTable, s->f_mntfromname, s->f_mntonname, *s);
#else
        rv = disks_table_add_row_statfs(pThd, pTable, s->f_mntfromname, s->f_mntonname, *s);
#endif
        count--;
	s++;
    }
    return rv;
}
#else /* HAVE_GETMNTINFO */


/* HAVE_GETMNTENT */
int disks_fill_table(THD* pThd, TABLE_LIST* pTables, Item* pCond)
{
    int rv = 1;
#ifdef HAVE_GETMNTENT_R
    struct mntent ent;
    const size_t BUFFER_SIZE = 4096; // 4K should be sufficient.
    char* pBuffer;
#endif
    struct mntent* pEnt;
    FILE* pFile;
    TABLE* pTable = pTables->table;

    if (check_global_access(pThd, FILE_ACL, true))
      return 0;

    pFile = setmntent(MOUNTED, "r");

    if (!pFile)
        return 1;

#ifdef HAVE_GETMNTENT_R
    pBuffer = new (std::nothrow) char [BUFFER_SIZE];

    if (!pBuffer)
        goto end_endmntent;
#endif
    rv = 0;

    while ((rv == 0) &&
#ifdef HAVE_GETMNTENT_R
        (pEnt = getmntent_r(pFile, &ent, pBuffer, BUFFER_SIZE))
#else
        (pEnt = getmntent(pFile))
#endif
        )
    {
        // We only report the ones that refer to physical disks.
        if (pEnt->mnt_fsname[0] == '/')
        {
            rv = disks_table_add_row(pThd, pTable, pEnt->mnt_fsname, pEnt->mnt_dir);
        }
    }

#ifdef HAVE_GETMNTENT_R
    delete [] pBuffer;
#endif

end_endmntent:
    endmntent(pFile);

    return rv;
}
#endif /* HAVE_GETMNTINFO */

int disks_table_init(void *ptr)
{
    ST_SCHEMA_TABLE* pSchema_table = (ST_SCHEMA_TABLE*)ptr;

    pSchema_table->fields_info = disks_table_fields;
    pSchema_table->fill_table = disks_fill_table;
    return 0;
}

} // namespace Show

extern "C"
{

maria_declare_plugin(disks)
{
    MYSQL_INFORMATION_SCHEMA_PLUGIN,
    &disks_table_info,                 /* type-specific descriptor */
    "DISKS",                           /* table name */
    "Johan Wikman, Daniel Black",      /* author */
    "Disk space information",          /* description */
    PLUGIN_LICENSE_GPL,                /* license type */
    Show::disks_table_init,            /* init function */
    NULL,                              /* deinit function */
    0x0101,                            /* version = 1.1 */
    NULL,                              /* no status variables */
    NULL,                              /* no system variables */
    "1.2",                             /* String version representation */
    MariaDB_PLUGIN_MATURITY_STABLE     /* Maturity (see include/mysql/plugin.h)*/
}
mysql_declare_plugin_end;

}
