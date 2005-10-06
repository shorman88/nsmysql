/*
 * MySQL internal driver for AOLserver 3
 * Copyright (C) 2000-2001 Panoptic Computer Network
 * Dossy <dossy@panoptic.com>
 *
 * AOLserver    http://www.aolserver.com/
 * MySQL        http://www.tcx.se/
 *
 * This was written and tested with:
 * - AOLserver 3.4.2, MySQL 3.23.36, x86 Linux 2.4.10 glibc 2.2.
 *
 * This driver is derived from the nssolid driver.
 */

static char     rcsid[] = "$Id: mysql.c,v 1.6 2005/10/06 20:16:33 shmooved Exp $";

#include "ns.h"
#include "nsdb.h"

/* MySQL API headers */
#include <mysql.h>
extern void my_thread_end(void);

/* Common system headers */
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#define MAX_ERROR_MSG	1024
#define MAX_IDENTIFIER	1024

static char    *mysql_driver_name = "MySQL";
static char    *mysql_driver_version = "Panoptic MySQL Driver v0.6";

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

static void     Log(Ns_DbHandle *handle, MYSQL *mysql);

/* Include tablename in resultset?  Default is no. */
static int      include_tablenames = 0;

static Ns_DbProc mysqlProcs[] = {
    { DbFn_Name,         (void *) Ns_MySQL_Name },
    { DbFn_DbType,       (void *) Ns_MySQL_DbType },
    { DbFn_ServerInit,   (void *) Ns_MySQL_ServerInit },
    { DbFn_OpenDb,       (void *) Ns_MySQL_OpenDb },
    { DbFn_CloseDb,      (void *) Ns_MySQL_CloseDb },
    { DbFn_DML,          (void *) Ns_MySQL_DML },
    { DbFn_Select,       (void *) Ns_MySQL_Select },
    { DbFn_GetRow,       (void *) Ns_MySQL_GetRow },
    { DbFn_Flush,        (void *) Ns_MySQL_Flush },
    { DbFn_Cancel,       (void *) Ns_MySQL_Cancel },
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

    if (Ns_DbRegisterDriver(hDriver, &(mysqlProcs[0])) != NS_OK) {
        Ns_Log(Error,
            "Ns_MySQL_DriverInit(%s):  Could not register the %s driver.",
            hDriver, mysql_driver_name);
        return NS_ERROR;
    }

    Ns_Log (Notice, "Ns_MySQL_DriverInit(%s):  Loaded %s, built on %s at %s.",
    	hDriver, mysql_driver_version, __DATE__, __TIME__);

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
    char            *datasource;
    char            *host = NULL;
    char            *database = NULL;
    char            *port = NULL;
    unsigned int    tcp_port = 0;
    char           *unix_socket = NULL;
    unsigned int    client_flag = 0;
    unsigned int    x, y, len;

    assert(handle != NULL);
    assert(handle->datasource != NULL);

    /* handle->datasource = "host:port:database" */
    datasource = ns_malloc(strlen(handle->datasource) + 1);
    strcpy(datasource, handle->datasource);
    host = datasource;
    for (port = host; port != NULL && *port != ':'; port++);
    *port = '\0';
    port++;
    for (database = port; database != NULL && *database != ':'; database++);
    *database = '\0';
    database++;

    if (host == NULL || port == NULL || database == NULL) {
        Ns_Log(Error, "Ns_MySQL_OpenDb(%s): '%s' is an invalid datasource string.", handle->driver, handle->datasource);
        ns_free(datasource);
        return NS_ERROR;
    }

    tcp_port = atoi(port);

    dbh = mysql_init(NULL);
    if (dbh == NULL) {
        Ns_Log(Error, "Ns_MySQL_OpenDb(%s): mysql_init() failed.",
            handle->datasource);
        ns_free(datasource);
        return NS_ERROR;
    }

    Ns_Log(Notice, "mysql_real_connect(%s, %s, %s, %s, %s)",
        host,
        handle->user == NULL ? "(null)" : handle->user,
        handle->password == NULL ? "(null)" : handle->password,
        database,
        port);

    if (! mysql_real_connect(dbh, host, handle->user, handle->password,
        database, tcp_port, unix_socket, client_flag)) {

        Log(handle, dbh);
        mysql_close(dbh);
        ns_free(datasource);
        return NS_ERROR;
    }

    ns_free(datasource);

    handle->connection = (void *) dbh;
    handle->connected = NS_TRUE;

    return NS_OK;
}

static int
Ns_MySQL_CloseDb(Ns_DbHandle *handle)
{
    assert(handle != NULL);

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_CloseDb(%s) called.", handle->datasource);

    mysql_close((MYSQL *) handle->connection);
    handle->connected = NS_FALSE;

    /*
     * From http://www.mysql.com/documentation/mysql/bychapter/manual_Clients.html#mysql_thread_end
     *
     * This function needs to be called before calling pthread_exit() to free
     * memory allocated by mysql_thread_init(). 
     *
     * Note that this function is not invoked automatically by the client
     * library.  It must be called explicitly to avoid a memory leak. 
     */

    /* And, the documentation is wrong.  It's called my_thread_end().  Sigh. */
    my_thread_end();

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_CloseDb(%s): closed successfully.",
            handle->datasource);

    return NS_OK;
}

static int
Ns_MySQL_DML(Ns_DbHandle *handle, char *sql)
{
    int             rc;

    assert(handle != NULL);
    assert(handle->connection != NULL);
    assert(sql != NULL);

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_DML(%s) called.", handle->datasource);

    rc = mysql_query((MYSQL *) handle->connection, sql);
    Log(handle, (MYSQL *) handle->connection);

    if (rc) {
        return NS_ERROR;
    }

    return NS_OK;
}

static Ns_Set  *
Ns_MySQL_Select(Ns_DbHandle *handle, char *sql)
{
    MYSQL_RES      *result;   
    MYSQL_FIELD    *fields;
    int             rc;
    unsigned int    i;
    unsigned int    numcols;
    Ns_DString     key;

    assert(handle != NULL);
    assert(handle->connection != NULL);
    assert(sql != NULL);

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_Select(%s) called.", handle->datasource);

    rc = mysql_query((MYSQL *) handle->connection, sql);
    Log(handle, (MYSQL *) handle->connection);

    if (rc) {
        return NULL;
    }

    result = mysql_store_result((MYSQL *) handle->connection);
    Log(handle, (MYSQL *) handle->connection);

    if (result == NULL) {
        return NULL;
    }

    handle->statement = (void *) result;
    handle->fetchingRows = NS_TRUE;

    numcols = mysql_num_fields((MYSQL_RES *) handle->statement);
    Log(handle, (MYSQL *) handle->connection);

    if (numcols == 0) {
        Ns_Log(Error, "Ns_MySQL_Select(%s):  Query did not return rows:  %s",
           handle->datasource, sql);

        mysql_free_result((MYSQL_RES *) handle->statement);
        handle->statement = NULL;
        handle->fetchingRows = NS_FALSE;
        return NULL;
    }

    numcols = mysql_num_fields((MYSQL_RES *) handle->statement);
    fields = mysql_fetch_fields((MYSQL_RES *) handle->statement);

    for (i = 0; i < numcols; i++) {
        Ns_DStringInit(&key);

        if (include_tablenames && strlen(fields[i].table) > 0) {
            Ns_DStringVarAppend(&key, fields[i].table, ".", NULL);
        }

        Ns_DStringAppend(&key, fields[i].name);

        Ns_SetPut((Ns_Set *) handle->row, Ns_DStringValue(&key), NULL);

        Ns_DStringFree(&key);
    }

    return (Ns_Set *) handle->row;
}

static int
Ns_MySQL_GetRow(Ns_DbHandle *handle, Ns_Set *row)
{
    MYSQL_ROW       my_row;
    int             i;
    int             numcols;

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_GetRow(%s) called.", handle->datasource);

    if (handle->fetchingRows == NS_FALSE) {
        Ns_Log(Error, "Ns_MySQL_GetRow(%s):  No rows waiting to fetch.",
            handle->datasource);
        return NS_ERROR;
    }

    numcols = mysql_num_fields((MYSQL_RES *) handle->statement);
    Log(handle, (MYSQL *) handle->connection);

    if (numcols == 0) {
        mysql_free_result((MYSQL_RES *) handle->statement);
        handle->statement = NULL;
        handle->fetchingRows = NS_FALSE;
        return NS_ERROR;
    }

    if (numcols != Ns_SetSize(row)) {
        Ns_Log(Error, "Ns_MySQL_GetRow: Number of columns in row (%d)"
            " not equal to number of columns in row fetched (%d).",
            Ns_SetSize(row), numcols);
        mysql_free_result((MYSQL_RES *) handle->statement);
        handle->statement = NULL;
        handle->fetchingRows = NS_FALSE;
        return NS_ERROR;
    }

    my_row = mysql_fetch_row((MYSQL_RES *) handle->statement);
    Log(handle, (MYSQL *) handle->connection);

    if (my_row == NULL) {
        mysql_free_result((MYSQL_RES *) handle->statement);
        handle->statement = NULL;
        handle->fetchingRows = NS_FALSE;
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
    assert(handle != NULL);
    assert(handle->connection != NULL);

    if (handle->fetchingRows == NS_TRUE) {
        MYSQL_RES      *result;

        result = (MYSQL_RES *) handle->statement;

        assert(result != NULL);

        /*
         * TODO:  I'm not sure what is supposed to happen here, so I'll
         * just dispose of the statement.  Now that MySQL supports
         * transactions, we probably should be issuing a ROLLBACK, too.
         */
        mysql_free_result(result);
        handle->statement = NULL;
        handle->fetchingRows = NS_FALSE;
    }

    return NS_OK;
}


static int
Ns_MySQL_Exec(Ns_DbHandle *handle, char *sql)
{
    MYSQL_RES      *result;
    int             rc;
    unsigned int    numcols, fieldcount;

    assert(handle != NULL);
    assert(handle->connection != NULL);
    assert(sql != NULL);

    if (handle->verbose) {
        Ns_Log(Notice, "Ns_MySQL_Exec(%s) called.", handle->datasource);
        Ns_Log(Notice, "Ns_MySQL_Exec(sql) = '%s'", sql);
    }

    rc = mysql_query((MYSQL *) handle->connection, sql);
    Log(handle, (MYSQL *) handle->connection);

    if (rc) {
        return NS_ERROR;
    }

    result = mysql_store_result((MYSQL *) handle->connection);
    Log(handle, (MYSQL *) handle->connection);

    fieldcount = mysql_field_count((MYSQL *) handle->connection);
    Log(handle, (MYSQL *) handle->connection);

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
        handle->fetchingRows = NS_TRUE;

        if (handle->verbose)
            Ns_Log(Notice, "Ns_MySQL_Exec(status) = NS_ROWS");

        return NS_ROWS;
    } else {
        mysql_free_result(result);

        if (handle->verbose)
            Ns_Log(Notice, "Ns_MySQL_Exec(status) = NS_DML");

        return NS_DML;
    }

    /* How did we get here? */
    return NS_ERROR;
}

static Ns_Set  *
Ns_MySQL_BindRow(Ns_DbHandle *handle)
{
    MYSQL_FIELD    *fields;
    unsigned int    i;
    unsigned int    numcols;
    Ns_DString     key;

    assert(handle != NULL);
    assert(handle->statement != NULL);

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_BindRow(%s) called.", handle->datasource);

    numcols = mysql_num_fields((MYSQL_RES *) handle->statement);

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_BindRow(numcols) = %u", numcols);

    fields = mysql_fetch_fields((MYSQL_RES *) handle->statement);

    for (i = 0; i < numcols; i++) {
        Ns_DStringInit(&key);

        if (include_tablenames && strlen(fields[i].table) > 0) {
            Ns_DStringVarAppend(&key, fields[i].table, ".", NULL);
        }

        Ns_DStringAppend(&key, fields[i].name);

        Ns_SetPut((Ns_Set *) handle->row, Ns_DStringValue(&key), NULL);

        Ns_DStringFree(&key);
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

    assert(handle != NULL);
    assert(handle->connection != NULL);

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_List_Dbs(%s) called.", handle->datasource);

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

    assert(handle != NULL);
    assert(handle->connection != NULL);

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_List_Tables(%s) called.", handle->datasource);

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

    assert(handle != NULL);
    assert(handle->connection != NULL);

    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_Select_Db(%s) called.", db);

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
    INT64            rows;
    char            *rows_str;

    assert(handle != NULL);
    assert(handle->connection != NULL);
    
    if (handle->verbose)
        Ns_Log(Notice, "Ns_MySQL_Resultrows(%s) called.", handle->datasource);

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
    sprintf(rows_str, NS_INT_64_FORMAT_STRING, rows);

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

    /*
     * Make sure this is a MySQL handle before accessing
     * handle->connection.
     */

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
    Ns_LogSeverity  severity = Error;
    unsigned int    nErr;
    char            msg[MAX_ERROR_MSG + 1];

    nErr = mysql_errno(mysql);
    if (nErr) {
        strncpy(msg, mysql_error(mysql), MAX_ERROR_MSG);
        Ns_Log(severity, "MySQL log message: (%u) '%s'", nErr, msg);

        if (handle != NULL) {
            sprintf(handle->cExceptionCode, "%u", nErr);
            Ns_DStringFree(&(handle->dsExceptionMsg));
            Ns_DStringAppend(&(handle->dsExceptionMsg), msg);
        }
    }
}

