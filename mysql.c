/*
 * MySQL internal driver for AOLserver 3
 * Copyright (C) 2000-2001 Panoptic Computer Network
 * Dossy <dossy@panoptic.com>
 *
 * AOLserver    http://www.aolserver.com/
 * MySQL        http://www.tcx.se/
 *
 * This was written and tested with:
 * - AOLserver 3.0 beta 5, MySQL 3.22.30, Linux 2.2.13 glibc 2.1.
 * - AOLserver 3.0rc1, MySQL 3.22.30, Linux 2.2.14 glibc 2.1.
 * - AOLserver 3.0rc1, MySQL 3.22.30, Win98.
 * - AOLserver 3.1 beta, MySQL 3.22.30, Linux 2.2.14 glibc 2.1.
 * - AOLserver 3.2, MySQL 3.22.30, Linux 2.2.17 glibc 2.2.
 *
 * This driver is derived from the nssolid driver.
 */

static char     rcsid[] = "$Id: mysql.c,v 1.2 2001/02/18 02:37:27 dossy Exp $";

#include "ns.h"

/* MySQL API headers */
#include <mysql.h>

/* Common system headers */
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#define MAX_ERROR_MSG 500
#define MAX_IDENTIFIER 256

static char    *mysql_driver_name = "MySQL";
static char    *mysql_driver_version = "Panoptic MySQL Driver v0.5";

static char    *Ns_MySQL_Name(void);
static char    *Ns_MySQL_DbType(Ns_DbHandle *handle);
static int      Ns_MySQL_ServerInit(char *hServer, char *hModule,
                                    char *hDriver);
static int      Ns_MySQL_OpenDb(Ns_DbHandle *handle);
static int      Ns_MySQL_CloseDb(Ns_DbHandle *handle);
static int      Ns_MySQL_DML(Ns_DbHandle *handle, char *sql);
static Ns_Set  *Ns_MySQL_Select(Ns_DbHandle *handle, char *sql);
static int      Ns_MySQL_GetRow(Ns_DbHandle *handle, Ns_Set *row);
static int      Ns_MySQL_Flush(Ns_DbHandle *handle);
static int      Ns_MySQL_Cancel(Ns_DbHandle *handle);
static int      Ns_MySQL_Exec(Ns_DbHandle *handle, char *sql);
static Ns_Set  *Ns_MySQL_BindRow(Ns_DbHandle *handle);

/* static MYSQL    global_handle; */
static int      Ns_MySQL_Shutdown(MYSQL *mysql);
static void     Log(Ns_DbHandle *handle, MYSQL *mysql);

/* Include tablename in resultset?  Default is yes. */
static int      include_tablenames = 1;

/*
 * thread safety mutex for the following unsafe MySQL operation:
 * mysql_query() / mysql_store_result()
 */

static Ns_Mutex mysql_lock;

static Ns_DbProc mysqlProcs[] = {
    { DbFn_Name,         Ns_MySQL_Name },
    { DbFn_DbType,       Ns_MySQL_DbType },
    { DbFn_ServerInit,   (void *) Ns_MySQL_ServerInit },
    { DbFn_OpenDb,       Ns_MySQL_OpenDb },
    { DbFn_CloseDb,      Ns_MySQL_CloseDb },
    { DbFn_DML,          Ns_MySQL_DML },
    { DbFn_Select,       Ns_MySQL_Select },
    { DbFn_GetRow,       Ns_MySQL_GetRow },
    { DbFn_Flush,        Ns_MySQL_Flush },
    { DbFn_Cancel,       Ns_MySQL_Cancel },
    { DbFn_Exec,         (void *) Ns_MySQL_Exec },
    { DbFn_BindRow,      (void *) Ns_MySQL_BindRow },
    { 0, NULL }
};


DllExport int   Ns_ModuleVersion = 1;
DllExport int   Ns_ModuleFlags = 0;
DllExport int
Ns_DbDriverInit(char *hDriver, char *configPath)
{
    if (hDriver == NULL) {
        Ns_Log(Bug, "Ns_MySQL_DriverInit():  NULL driver name.");
        return NS_ERROR;
    }

    /* initialize the mutex lock */
    Ns_MutexSetName(&mysql_lock, "mysql_lock");

    /*
    if (mysql_init(&dbh) == NULL) {
        Ns_Log(Error,
            "Ns_MySQL_DriverInit(%s):  Could not initialize the %s driver.",
            hDriver, mysql_driver_name);
        return NS_ERROR;
    }
    */

    if (Ns_DbRegisterDriver(hDriver, &(mysqlProcs[0])) != NS_OK) {
        Ns_Log(Error,
            "Ns_MySQL_DriverInit(%s):  Could not register the %s driver.",
            hDriver, mysql_driver_name);
        return NS_ERROR;
    }

    Ns_Log (Notice, "Ns_MySQL_DriverInit(%s):  Loaded %s, built on %s at %s.",
    	hDriver, mysql_driver_version, __DATE__, __TIME__);

    /*
    Ns_RegisterShutdown((Ns_Callback *) Ns_MySQL_Shutdown, &global_handle);
    */

    return NS_OK;
}

static int
Ns_MySQL_Shutdown(MYSQL *mysql)
{
    mysql_close(mysql);
    return NS_OK;
}

static char    *
Ns_MySQL_Name(void)
{
    return mysql_driver_name;
}


static char    *
Ns_MySQL_DbType(Ns_DbHandle *handle)
{
    char            buf[MAX_IDENTIFIER + 1];

    /* should probably grab a mutex here as well, this is scary. */

    if ((MYSQL *) handle->connection != NULL)
        sprintf(buf, "%.100s %.300s", mysql_driver_name,
            mysql_get_server_info((MYSQL *) handle->connection));
    else
        sprintf(buf, "%.100s", mysql_driver_name);

    /*
     * FIXME: THIS IS UGLY -- does AOLserver offer a cleaner way of
     * doing this?  Sometimes, garbage collection is nice ...
     */
    return strdup(buf);
}

static int
Ns_MySQL_OpenDb(Ns_DbHandle *handle)
{
    MYSQL          *dbh;
    int             rc;
    char            host[128];
    char            db[128];
    char            _port[128];
    unsigned int    tcp_port;
    char           *unix_socket;
    unsigned int    client_flag;
    unsigned int    x, y, len;

    assert(handle != NULL);
    assert(handle->datasource != NULL);

    /* handle->datasource = "host:port:database" */
    db[0] = '\0';
    host[0] = '\0';
    tcp_port = 0;
    unix_socket = NULL;
    client_flag = 0;

    len = strlen(handle->datasource);
    for (x = 0, y = 0; x < len; x++, y++) {
        if (handle->datasource[x] == ':')
            break;
        host[y] = handle->datasource[x];
    }
    host[y] = '\0';

    for (x++, y = 0; x < len; x++, y++) {
        if (handle->datasource[x] == ':')
            break;
        _port[y] = handle->datasource[x];
    }
    _port[y] = '\0';
    tcp_port = atoi(_port);

    for (x++, y = 0; x < len; x++, y++) {
        db[y] = handle->datasource[x];
    }
    db[y] = '\0';

    client_flag = 0;

    /* start critical section */
    Ns_MutexLock(&mysql_lock);

    dbh = mysql_init(NULL);
    if (dbh == NULL) {
    	Ns_MutexUnlock(&mysql_lock);
        return NS_ERROR;
    }

    Ns_Log(Notice, "mysql_real_connect(%s, %s, %s)", host,
        handle->user == NULL ? "(null)" : handle->user,
        handle->password == NULL ? "(null)" : handle->password);

    if (! mysql_real_connect(dbh, host, handle->user,
        handle->password, db, tcp_port, unix_socket, client_flag)) {
    	Log(handle, dbh);
    	Ns_MutexUnlock(&mysql_lock);
        return NS_ERROR;
    }

    handle->connection = (void *) dbh;
    handle->connected = NS_TRUE;

    Ns_Log(Notice, "mysql_select_db(%s)", db);
    rc = mysql_select_db((MYSQL *) handle->connection, db);
    Log(handle, (MYSQL *) handle->connection);
    if (rc) {
    	Ns_MutexUnlock(&mysql_lock);
        return NS_ERROR;
    }

    Ns_MutexUnlock(&mysql_lock);
    /* end critical section */

    return NS_OK;
}

static int
Ns_MySQL_CloseDb(Ns_DbHandle *handle)
{
    mysql_close((MYSQL *) handle->connection);
    handle->connected = NS_FALSE;
    return NS_OK;
}

static int
Ns_MySQL_DML(Ns_DbHandle *handle, char *sql)
{
    int             rc;
    int             status;

    assert(handle != NULL);
    assert(sql != NULL);

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_DML called.");

    status = NS_OK;

    /* start critical section */
    Ns_MutexLock(&mysql_lock);

    rc = mysql_query((MYSQL *) handle->connection, sql);
    Log(handle, (MYSQL *) handle->connection);
    if (rc) {
    	Ns_MutexUnlock(&mysql_lock);
        return NS_ERROR;
    }

    Ns_MutexUnlock(&mysql_lock);
    /* end critical section */

    return status;
}

static Ns_Set  *
Ns_MySQL_Select(Ns_DbHandle *handle, char *sql)
{
    MYSQL_RES      *result;   
    MYSQL_FIELD    *fields;
    int             rc;
    unsigned int    i;
    unsigned int    numcols;
    Tcl_DString     key;

    assert(handle != NULL);
    assert(sql != NULL);

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_Select called.");

    /* start critical section */
    Ns_MutexLock(&mysql_lock);

    rc = mysql_query((MYSQL *) handle->connection, sql);
    Log(handle, (MYSQL *) handle->connection);
    if (rc) {
    	Ns_MutexUnlock(&mysql_lock);
        return NULL;
    }

    result = mysql_store_result((MYSQL *) handle->connection);
    Log(handle, (MYSQL *) handle->connection);
    if (result == NULL) {
    	Ns_MutexUnlock(&mysql_lock);
        return NULL;
    }

    handle->statement = (void *) result;
    handle->fetchingRows = 1;

    numcols = mysql_num_fields((MYSQL_RES *) handle->statement);
    Log(handle, (MYSQL *) handle->connection);
    if (numcols == 0) {
        Ns_Log(Error, "Ns_MySQL_Select(%s):  Query did not return rows:  %s",
           handle->datasource, sql);

        mysql_free_result((MYSQL_RES *) handle->statement);
        handle->statement = NULL;
        handle->fetchingRows = 0;
    	Ns_MutexUnlock(&mysql_lock);
        return NULL;
    }

    numcols = mysql_num_fields((MYSQL_RES *) handle->statement);
    fields = mysql_fetch_fields((MYSQL_RES *) handle->statement);
    for (i = 0; i < numcols; i++) {
        Tcl_DStringInit(&key);

        if (include_tablenames && strlen(fields[i].table) > 0) {
            Tcl_DStringAppend(&key, fields[i].table, strlen(fields[i].table));
            Tcl_DStringAppend(&key, ".", 1);
        }
        Tcl_DStringAppend(&key, fields[i].name, strlen(fields[i].name));

        Ns_SetPut((Ns_Set *) handle->row, Tcl_DStringValue(&key), NULL);

        Tcl_DStringFree(&key);
    }

    Ns_MutexUnlock(&mysql_lock);
    /* end critical section */

    return (Ns_Set *) handle->row;
}

static int
Ns_MySQL_GetRow(Ns_DbHandle *handle, Ns_Set *row)
{
    MYSQL_ROW       my_row;
    int             i;
    int             numcols;

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_GetRow called.");

    if (!handle->fetchingRows) {
        Ns_Log(Error, "Ns_MySQL_GetRow(%s):  No rows waiting to fetch.",
            handle->datasource);
        return NS_ERROR;
    }

    numcols = mysql_num_fields((MYSQL_RES *) handle->statement);
    Log(handle, (MYSQL *) handle->connection);
    if (numcols == 0) {
error:
        mysql_free_result((MYSQL_RES *) handle->statement);
        handle->statement = NULL;
        handle->fetchingRows = 0;
        return NS_ERROR;
    }

    if (numcols != Ns_SetSize(row)) {
        Ns_Log(Error, "Ns_MySQL_GetRow: Number of columns in row (%d)"
            " not equal to number of columns in row fetched (%d)",
            Ns_SetSize(row), numcols);
        goto error;
    }

    my_row = mysql_fetch_row((MYSQL_RES *) handle->statement);
    Log(handle, (MYSQL *) handle->connection);

    if (my_row == NULL) {
        mysql_free_result((MYSQL_RES *) handle->statement);
        handle->statement = NULL;
        handle->fetchingRows = 0;
        return NS_END_DATA;
    }

    for (i = 0; i < numcols; i++) {
        if (my_row[i] == NULL) {
            Ns_SetPutValue(row, i, "");
        } else {
            Ns_SetPutValue(row, i, my_row[i]);
        }
    }

    return NS_OK;
}

static int
Ns_MySQL_Flush(Ns_DbHandle *handle)
{
    return Ns_MySQL_Cancel(handle);
}

static int
Ns_MySQL_Cancel(Ns_DbHandle *handle)
{
    if (handle->fetchingRows) {
        MYSQL_RES      *result;

        result = (MYSQL_RES *) handle->statement;

        assert(result != NULL);

        /*
         * I'm not sure what is supposed to happen here, so I'll just
         * dispose of the statement.
         */
        mysql_free_result(result);
        handle->statement = NULL;
        handle->fetchingRows = 0;
    }
    return NS_OK;
}


static int
Ns_MySQL_Exec(Ns_DbHandle *handle, char *sql)
{
    MYSQL_RES      *result;
    int             rc;
    int             status;
    unsigned int    numcols, fieldcount;

    assert(handle != NULL);
    assert(sql != NULL);

    if (handle->verbose) {
        Ns_Log(Notice, "Ns_MySQL_Exec called.");
        Ns_Log(Notice, "Ns_MySQL_Exec(sql) = '%s'", sql);
    }

    status = NS_OK;

    /* start critical section */
    Ns_MutexLock(&mysql_lock);

    rc = mysql_query((MYSQL *) handle->connection, sql);
    Log(handle, (MYSQL *) handle->connection);
    if (rc) {
    	Ns_MutexUnlock(&mysql_lock);
        return NS_ERROR;
    }

    result = mysql_store_result((MYSQL *) handle->connection);
    Log(handle, (MYSQL *) handle->connection);

    fieldcount = mysql_field_count((MYSQL *) handle->connection);
    Log(handle, (MYSQL *) handle->connection);

    Ns_MutexUnlock(&mysql_lock);
    /* end critical section */

    if (result == NULL) {
    	if (fieldcount == 0) {	
    		if (handle->verbose)
    			Ns_Log(Notice, "Ns_MySQL_Exec(status) = NS_DML");
    		return NS_DML;
    	} else {
    		Ns_Log(Error, "Ns_MySQL_Exec() has columns but result set is NULL");
    		return NS_ERROR;
    	}
    }

    numcols = mysql_num_fields(result);
    Log(handle, (MYSQL *) handle->connection);
    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_Exec(numcols) = %u", numcols);
    if (numcols != 0) {
        handle->statement = (void *) result;
        handle->fetchingRows = 1;
        status = NS_ROWS;
        if (handle->verbose)
            Ns_Log(Notice, "Ns_MySQL_Exec(status) = NS_ROWS");
    } else {
        mysql_free_result(result);
        status = NS_DML;
        if (handle->verbose)
            Ns_Log(Notice, "Ns_MySQL_Exec(status) = NS_DML");
    }

    return status;
}

static Ns_Set  *
Ns_MySQL_BindRow(Ns_DbHandle *handle)
{
    MYSQL_FIELD    *fields;
    unsigned int    i;
    unsigned int    numcols;
    Tcl_DString     key;

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_BindRow called.");

    numcols = mysql_num_fields((MYSQL_RES *) handle->statement);
    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_BindRow(numcols) = %u", numcols);

    fields = mysql_fetch_fields((MYSQL_RES *) handle->statement);
    for (i = 0; i < numcols; i++) {
        Tcl_DStringInit(&key);

        if (include_tablenames && strlen(fields[i].table) > 0) {
            Tcl_DStringAppend(&key, fields[i].table, strlen(fields[i].table));
            Tcl_DStringAppend(&key, ".", 1);
        }
        Tcl_DStringAppend(&key, fields[i].name, strlen(fields[i].name));

        Ns_SetPut((Ns_Set *) handle->row, Tcl_DStringValue(&key), NULL);

        Tcl_DStringFree(&key);
    }

    return (Ns_Set *) handle->row;
}

/* ************************************************************ */

static int 
Ns_MySQL_List_Dbs(Tcl_Interp *interp, const char *wild, Ns_DbHandle *handle)
{
    MYSQL_RES      *result;
    MYSQL_ROW       row;
    unsigned int    numcols;
    unsigned int    i;

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_List_Dbs");

    result = mysql_list_dbs((MYSQL *) handle->connection, wild);
    Log(handle, (MYSQL *) handle->connection);
    if (result == NULL) {
        Tcl_AppendResult(interp, "mysql_list_dbs failed.", NULL);
        return TCL_ERROR;
    }

    numcols = mysql_num_fields(result);
    Log(handle, (MYSQL *) handle->connection);

    while ((row = mysql_fetch_row(result)) != NULL) {
        for (i = 0; i < numcols; i++) {
            Tcl_AppendElement(interp, row[i]);
        }
    }

    mysql_free_result(result);

    return TCL_OK;
}

static int 
Ns_MySQL_List_Tables(Tcl_Interp *interp, const char *wild, Ns_DbHandle *handle)
{
    MYSQL_RES      *result;
    MYSQL_ROW       row;
    unsigned int    numcols;
    unsigned int    i;

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_List_Tables");

    result = mysql_list_tables((MYSQL *) handle->connection, wild);
    Log(handle, (MYSQL *) handle->connection);
    if (result == NULL) {
        Tcl_AppendResult(interp, "mysql_list_tables failed.", NULL);
        return TCL_ERROR;
    }

    numcols = mysql_num_fields(result);
    Log(handle, (MYSQL *) handle->connection);

    while ((row = mysql_fetch_row(result)) != NULL) {
        for (i = 0; i < numcols; i++) {
            Tcl_AppendElement(interp, row[i]);
        }
    }

    mysql_free_result(result);

    return TCL_OK;
}

static int 
Ns_MySQL_Select_Db(Tcl_Interp *interp, const char *db, Ns_DbHandle *handle)
{
    unsigned int    rc;

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_Select_Db(%s)", db);

    rc = mysql_select_db((MYSQL *) handle->connection, db);
    Log(handle, (MYSQL *) handle->connection);
    if (rc) {
        Tcl_AppendResult(interp, "mysql_select_db failed.", NULL);
        return TCL_ERROR;
    }

    Tcl_SetResult(interp, (char *) db, TCL_STATIC);
    return TCL_OK;
}

static int 
Ns_MySQL_Resultrows(Tcl_Interp *interp, Ns_DbHandle *handle)
{
#ifdef WIN32
    unsigned __int64        rows;
#else
    unsigned long long      rows;
#endif
    char                    *rows_str;

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_Resultrows()");

    rows = mysql_affected_rows((MYSQL *) handle->connection);
    Log(handle, (MYSQL *) handle->connection);
    if (rows < 0) {
        Tcl_AppendResult(interp, "mysql_affected_rows failed.", NULL);
        return TCL_ERROR;
    }

    /*
     * an unsigned long long (64 bits) will never have a character
     * representation longer than 20 characters.
     */
    rows_str = Tcl_Alloc(21);
    sprintf(rows_str, "%llu", rows);

    Tcl_SetResult(interp, rows_str, TCL_DYNAMIC);
    return TCL_OK;
}

/*
 * Ns_MySQL_Cmd - This function implements the "ns_mysql" Tcl command
 * installed into each interpreter of each virtual server.  It provides
 * access to features specific to the MySQL driver.
 */

static int
Ns_MySQL_Cmd(ClientData dummy, Tcl_Interp *interp, int argc, char **argv)
{
    Ns_DbHandle    *handle;

    if (argc < 3 || argc > 4) {
        Tcl_AppendResult(interp, "wrong # args: should be \"",
            argv[0], " cmd handle ?args?\"", NULL);
        return TCL_ERROR;
    }
    if (Ns_TclDbGetHandle(interp, argv[2], &handle) != TCL_OK) {
        return TCL_ERROR;
    }

    /* Make sure this is a MySQL handle before accessing handle->connection. */
    if (Ns_DbDriverName(handle) != mysql_driver_name) {
        Tcl_AppendResult(interp, "handle \"", argv[1], "\" is not of type \"",
            mysql_driver_name, "\"", NULL);
        return TCL_ERROR;
    }

    if (STREQ(argv[1], "include_tablenames")) {
        /* == [ns_mysql include_tablenames $db (on|off)] == */
        if (argc != 4) {
            Tcl_AppendResult(interp, "wrong # args: should be \"",
                argv[0], " include_tablenames handle boolean\"", NULL);
            return TCL_ERROR;
        }
        return Tcl_GetBoolean(interp, argv[3], &include_tablenames);
    } else if (STREQ(argv[1], "list_dbs")) {
        /* == [ns_mysql list_dbs $db ?wild?] == */
        char           *wild;

        if (argc < 3 || argc > 4) {
            Tcl_AppendResult(interp, "wrong # args: should be \"",
                argv[0], " list_dbs handle ?wild?\"", NULL);
            return TCL_ERROR;
        }
        if (argc == 3) {
            wild = NULL;
        } else {
            wild = argv[3];
        }
        return Ns_MySQL_List_Dbs(interp, wild, handle);
    } else if (STREQ(argv[1], "list_tables")) {
        /* == [ns_mysql list_tables $db ?wild?] == */
        char           *wild;

        if (argc < 3 || argc > 4) {
            Tcl_AppendResult(interp, "wrong # args: should be \"",
                argv[0], " list_tables handle ?wild?\"", NULL);
            return TCL_ERROR;
        }
        if (argc == 3) {
            wild = NULL;
        } else {
            wild = argv[3];
        }
        return Ns_MySQL_List_Tables(interp, wild, handle);
    } else if (STREQ(argv[1], "resultrows")) {
        /* == [ns_mysql resultrows $db] == */
        if (argc != 3) {
            Tcl_AppendResult(interp, "wrong # args: should be \"",
                argv[0], " resultrows handle\"", NULL);
            return TCL_ERROR;
        }
        return Ns_MySQL_Resultrows(interp, handle);
    } else if (STREQ(argv[1], "select_db")) {
        /* == [ns_mysql select_db $db database] == */
        if (argc != 4) {
            Tcl_AppendResult(interp, "wrong # args: should be \"",
                argv[0], " select_db handle database\"", NULL);
            return TCL_ERROR;
        }
        return Ns_MySQL_Select_Db(interp, argv[3], handle);
    } else if (STREQ(argv[1], "version")) {
        /* == [ns_mysql version $db] == */
        if (argc != 3) {
            Tcl_AppendResult(interp, "wrong # args: should be \"",
                argv[0], " version handle\"", NULL);
            return TCL_ERROR;
        }
        Tcl_SetResult(interp, mysql_driver_version, TCL_STATIC);
        return TCL_OK;
    } else {
        Tcl_AppendResult(interp, "unknown command \"", argv[1],
            "\": should be list_dbs, list_tables, select_db, or "
            "version.", NULL);
        return TCL_ERROR;
    }
    
    return TCL_OK;
}


static int
Ns_MySQLInterpInit(Tcl_Interp *interp, void *ignored)
{
    Tcl_CreateCommand(interp, "ns_mysql", Ns_MySQL_Cmd, NULL, NULL);
    return NS_OK;
}


static int
Ns_MySQL_ServerInit(char *hServer, char *hModule, char *hDriver)
{
    return Ns_TclInitInterps(hServer, Ns_MySQLInterpInit, NULL);
}


static void
Log(Ns_DbHandle *handle, MYSQL *mysql)
{
    Ns_LogSeverity  severity;
    unsigned int    nErr;

    nErr = mysql_errno(mysql);
    if (nErr) {
        char            msg[MAX_ERROR_MSG + 1];

        severity = Error;
        strncpy(msg, mysql_error(mysql), MAX_ERROR_MSG);
        Ns_Log(severity, "MySQL log message: (%u) '%s'", nErr, msg);
        if (handle != NULL) {
            strcpy(handle->cExceptionCode, "Error");
            Ns_DStringFree(&(handle->dsExceptionMsg));
            Ns_DStringAppend(&(handle->dsExceptionMsg), msg);
        }
    }
}

