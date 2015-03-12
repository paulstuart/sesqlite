/* SeSqlite extension to add SELinux checks in SQLite */

#if !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_SELINUX)

#ifndef SQLITE_CORE
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#else
#include "sqlite3.h"
#endif

#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
#include <selinux/selinux.h>
#include "selinux.h"
#include "sesqlite_vtab.h"
#include "sesqlite_hash.h"
#include <time.h>

#define USE_AVC

/* source (process) security context */
security_context_t scon = 0;
security_context_t tcon = 0;
int scon_id = 0;
int tcon_id = 0;

/* prepared statements to query schema sesqlite_master (bind it before use) */
sqlite3_stmt *sesqlite_stmt;
sqlite3_stmt *sesqlite_stmt_id_insert;
sqlite3_stmt *sesqlite_stmt_id_select;
sqlite3_stmt *sesqlite_stmt_con_select;

/**
 * HashMap used by the virtual table implementation
 */
static seSQLiteHash hash; /* HashMap*/
static seSQLiteHash hash_id; /* HashMap used to map security_context -> int*/
static seSQLiteHash hash_id_revert; /* HashMap used to map security_context -> int*/

#ifdef USE_AVC
static seSQLiteHash avc; /* HashMap*/
#endif


/**
 *  * Function used to store the mapping between security_context and id in the
 *  'selinux_id' table.
 *   */
int insertId(sqlite3 *db, char *con){
    int rc = SQLITE_OK;
    int *res = 0;
    int rowid = 0;

    char *ttcon = sqlite3_mprintf("'%s'", con); /* remember, storing quoted string */
    sqlite3_bind_text(sesqlite_stmt_con_select, 1, con,
	strlen(con), SQLITE_TRANSIENT);

    while(sqlite3_step(sesqlite_stmt_con_select) == SQLITE_ROW){
	rowid = sqlite3_column_int(sesqlite_stmt_con_select, 0);
    }
    sqlite3_reset(sesqlite_stmt_con_select);
    if(rowid == 0){
	sqlite3_bind_text(sesqlite_stmt_id_insert, 1, con, strlen(con),
	    SQLITE_TRANSIENT);

	rc = sqlite3_step(sesqlite_stmt_id_insert);
	rc = sqlite3_reset(sesqlite_stmt_id_insert);
    
	rowid = sqlite3_last_insert_rowid(db);
	seSQLiteHashInsert(&hash_id, NULL, rowid, ttcon, 0, 0);
   }

   return rowid;
}

/*
 * Return the security context of the given (table), (table, column) element.
 */
/*
 * Function: getContext 
 * Purpose: Given the table name or table and column name, returns the security
 * context associated.
 * Parameters:
 * 		dbname: database name;
 * 		tclass: 1 = table, 2 = column;
 * 		table: table name;
 * 		column: column name, it is NULL if tclass == 1.
 * Return value: Return the security context associated to the table/column
 * given. 
 */
int getContext(sqlite3 *db, const char *dbname, int tclass, const char *table,
	const char *column) {

	char *key = NULL;
	int *res = 0;

	switch (tclass) {
	case 0: /* database */
		break;
	case 1: /* table */
		key = sqlite3_mprintf("%s:%s", dbname, table);
		break;
	case 2:
		key = sqlite3_mprintf("%s:%s:%s", dbname, table, column);
		break;
	}

	assert(key != NULL);
	res = seSQLiteHashFind(&hash, key, strlen(key));

	// Here we can free key since we're not inserting it in the hashtable.
	sqlite3_free(key);

	if (res != NULL) {
#ifdef SQLITE_DEBUG
fprintf(stdout, "Hash hint: db=%s, table=%s, column=%s -> %d\n", dbname, table,
    (column ? column : "NULL"), *res);
#endif
	    return *res;
	}else {
	    security_context_t security_context_new = 0;
	    int id = 0;
	    int *value = 0;
	    value = sqlite3_malloc(sizeof(int));
#ifdef SELINUX_STATIC_CONTEXT
    security_context_new = sqlite3_mprintf("%s", "unconfined_u:object_r:unconfined_t:s0");
#else
    if(security_compute_create_raw(scon, scon, (column ? SELINUX_DB_COLUMN : SELINUX_DB_TABLE), &security_context_new) < 0){
	fprintf(stderr, "SELinux could not compute a default context\n");
	return 0;
    }
#endif
	    id = insertId(db, security_context_new);
	    *value = id;
	    seSQLiteHashInsert(&hash, key, strlen(key), value, 0, 0);
#ifdef SQLITE_DEBUG
fprintf(stdout, "Compute New Context: db=%s, table=%s, column=%s -> %d\n", dbname, table,
    (column ? column : "NULL"), id);
#endif
	    return id; /* something wrong, a table/column was not labeled correctly */	
	}
}

/*
 * Checks whether the source context has been granted the specified permission
 * for the classes 'db_table' and 'db_column' and the target context associated with the table/column.
 * Returns 1 if the access has been granted, 0 otherwise.
 */
int checkAccess(sqlite3 *db, const char *dbname, const char *table, const char *column,
		int tclass, int perm) {

	assert(tclass <= NELEMS(access_vector));

	int id = getContext(db, dbname, tclass, table, column);
	assert(id != 0);

	unsigned int key = compress(scon_id,
		id,
		access_vector[tclass].c_code,
		access_vector[tclass].perm[perm].p_code);

	int *res = seSQLiteHashFind(&avc, NULL, key);
	if (res == NULL) {
		res = sqlite3_malloc(sizeof(int));
		char *ttcon = seSQLiteHashFind(&hash_id, NULL, id);
		sqlite3Dequote(ttcon);
		*res = selinux_check_access(scon, ttcon, access_vector[tclass].c_name,
		    access_vector[tclass].perm[perm].p_name, NULL);

		seSQLiteHashInsert(&avc, NULL, key, res, 0, 0);
	}

	// DO NOT FREE key AND res
	return 0 == *res;
}

/**
 * Scan all the columns and call checkAccess
 */
int checkAllColumns(sqlite3* pdb, const char *dbName, const char* tblName,
		int type, int action) {

	int rc = SQLITE_OK;
	int i = -1; /* Database number */
	int j;
	Hash *pTbls;
	HashElem *x;
	Db *pDb;

	// TODO type = db_column

	Table *pTab = sqlite3FindTable(pdb, tblName, dbName);
	if (pTab) {
		Column *pCol;
		for (j = 0, pCol = pTab->aCol; j < pTab->nCol; j++, pCol++) {
			if (!checkAccess(pdb, dbName, tblName, pCol->zName, type, action)) {
				return SQLITE_DENY;
			}
		}
	}

	return rc;
}

/*
 * Authorizer to be set with sqlite3_set_authorizer that checks the SELinux
 * permission at schema level (tables and columns).
 */
int selinuxAuthorizer(void *pUserData, int type, const char *arg1,
		const char *arg2, const char *dbname, const char *source) {
	int rc = SQLITE_OK;

	sqlite3 *pdb = (sqlite3*) pUserData;

//	if (!auth_enabled)
//		return rc;

#ifdef SQLITE_DEBUG
	//fprintf(stdout, "authorizer: type=%s arg1=%s arg2=%s\n", authtype[type],
	//		(arg1 ? arg1 : "NULL"), (arg2 ? arg2 : "NULL"));
#endif

	switch (type) /* arg1          | arg2            */
	{

	case SQLITE_CREATE_INDEX: /* Index Name    | Table Name      */
		break;

	case SQLITE_CREATE_TABLE: /* Table Name    | NULL            */
		//check if the sesqlite_master contains the security_context to assign,
		//otherwise use the default security_context.
		//TODO in android the default security_context is not always the same (e.g., untrusted_app->app_data_file,
		//release_app->platform_app_data_file)
		if (!checkAccess(pdb, dbname, arg1, NULL, SELINUX_DB_TABLE,
		SELINUX_CREATE)) {
			rc = SQLITE_DENY;
		}
		break;

	case SQLITE_CREATE_TEMP_INDEX: /* Index Name    | Table Name      */
		break;

	case SQLITE_CREATE_TEMP_TABLE: /* Table Name    | NULL            */
		//see SQLITE_CREATE_TABLE comment
		if (!checkAccess(pdb, dbname, arg1, NULL, SELINUX_DB_TABLE,
		SELINUX_CREATE)) {
			rc = SQLITE_DENY;
		}
		break;

	case SQLITE_CREATE_TEMP_TRIGGER: /* Trigger Name  | Table Name      */
		break;

	case SQLITE_CREATE_TEMP_VIEW: /* View Name     | NULL            */
		//TODO TABLE == VIEW??
		if (!checkAccess(pdb, dbname, arg1, NULL, SELINUX_DB_TABLE,
		SELINUX_CREATE)) {
			rc = SQLITE_DENY;
		}
		break;

	case SQLITE_CREATE_TRIGGER: /* Trigger Name  | Table Name      */
		if (!checkAccess(pdb, dbname, arg2, NULL, SELINUX_DB_TABLE,
		SELINUX_SETATTR)) {
			rc = SQLITE_DENY;
		}
		break;

	case SQLITE_CREATE_VIEW: /* View Name     | NULL            */
		//TODO TABLE == VIEW??
		if (!checkAccess(pdb, dbname, arg1, NULL, SELINUX_DB_TABLE,
		SELINUX_CREATE)) {
			rc = SQLITE_DENY;
		}
		break;

	case SQLITE_DELETE: /* Table Name    | NULL            */
		if (!checkAccess(pdb, dbname, arg1, NULL, SELINUX_DB_TABLE,
		SELINUX_DELETE)) {
			rc = SQLITE_DENY;
		}

		rc = checkAllColumns(pdb, dbname, arg1, SELINUX_DB_COLUMN,
		SELINUX_DROP);

		break;

	case SQLITE_DROP_INDEX: /* Index Name    | Table Name      */
		if (!checkAccess(pdb, dbname, arg2, NULL, SELINUX_DB_TABLE,
		SELINUX_SETATTR)) {
			rc = SQLITE_DENY;
		}
		break;

	case SQLITE_DROP_TABLE: /* Table Name    | NULL            */
		if (!checkAccess(pdb, dbname, arg1, NULL, SELINUX_DB_TABLE,
		SELINUX_DROP)) {
			rc = SQLITE_DENY;
		}

		rc = checkAllColumns(pdb, dbname, arg1, SELINUX_DB_COLUMN,
		SELINUX_DROP);

		break;

	case SQLITE_DROP_TEMP_INDEX: /* Index Name    | Table Name      */
		break;

	case SQLITE_DROP_TEMP_TABLE: /* Table Name    | NULL            */
		if (!checkAccess(pdb, dbname, arg1, NULL, SELINUX_DB_TABLE,
		SELINUX_DROP)) {
			rc = SQLITE_DENY;
		}

		rc = checkAllColumns(pdb, dbname, arg1, SELINUX_DB_COLUMN,
		SELINUX_DROP);

		break;

	case SQLITE_DROP_TEMP_TRIGGER: /* Trigger Name  | Table Name      */
		break;

	case SQLITE_DROP_TEMP_VIEW: /* View Name     | NULL            */
		if (!checkAccess(pdb, dbname, arg1, NULL, SELINUX_DB_TABLE,
		SELINUX_DROP)) {
			rc = SQLITE_DENY;
		}
		break;

	case SQLITE_DROP_TRIGGER: /* Trigger Name  | Table Name      */
		if (!checkAccess(pdb, dbname, arg2, NULL, SELINUX_DB_TABLE,
		SELINUX_SETATTR)) {
			rc = SQLITE_DENY;
		}
		break;

	case SQLITE_DROP_VIEW: /* View Name     | NULL            */
		if (!checkAccess(pdb, dbname, arg1, NULL, SELINUX_DB_TABLE,
		SELINUX_DROP)) {
			rc = SQLITE_DENY;
		}

		rc = checkAllColumns(pdb, dbname, arg1, SELINUX_DB_COLUMN,
		SELINUX_DROP);

		break;

	case SQLITE_INSERT: /* Table Name    | NULL            */
		if (!checkAccess(pdb, dbname, arg1, NULL, SELINUX_DB_TABLE,
		SELINUX_INSERT)) {
			rc = SQLITE_DENY;
		}

		rc = checkAllColumns(pdb, dbname, arg1, SELINUX_DB_COLUMN,
		SELINUX_INSERT);

		break;

	case SQLITE_PRAGMA: /* Pragma Name   | 1st arg or NULL */
		if (0 == sqlite3_stricmp(arg1, "writable_schema")) {
			fprintf(stderr,
					"Pragma disabled to guarantee SeSqlite checks. [pragma command: %s]\n",
					arg1);
			rc = SQLITE_DENY;
		}
		break;

	case SQLITE_READ: /* Table Name    | Column Name     */

		if (!checkAccess(pdb, dbname, arg1, arg2, SELINUX_DB_COLUMN,
		SELINUX_SELECT)) {
			rc = SQLITE_DENY;
		}

		break;

	case SQLITE_SELECT: /* NULL          | NULL            */
		break;

	case SQLITE_TRANSACTION: /* Operation     | NULL            */
		break;

	case SQLITE_UPDATE: /* Table Name    | Column Name     */
		if (!checkAccess(pdb, dbname, arg1, arg2, SELINUX_DB_COLUMN,
		SELINUX_UPDATE)) {
			rc = SQLITE_DENY;
		}

		break;

	case SQLITE_ATTACH: /* Filename      | NULL            */
		// TODO change when multiple databases are supported by SeSqlite.
		//modify access vector policy
		//UNION selinux_context
		if ((arg1 != NULL) && (strlen(arg1) != 0)) {
			fprintf(stderr,
					"SeSqlite does not support multiple databases yet. [db filename: %s]\n",
					arg1);
			rc = SQLITE_DENY;
		}
		break;

	case SQLITE_DETACH: /* Database Name | NULL            */
		// TODO change when multiple databases are supported by SeSqlite.
		if ((arg1 != NULL) && (strlen(arg1) != 0)) {
			fprintf(stderr,
					"SeSqlite does not support multiple databases yet. [db filename: %s]\n",
					arg1);
			rc = SQLITE_DENY;
		}
		break;

	case SQLITE_ALTER_TABLE: /* Database Name | Table Name      */
		if (!checkAccess(pdb, arg1, arg2, NULL, SELINUX_DB_TABLE,
		SELINUX_GETATTR)) {
			rc = SQLITE_DENY;
		}
		break;

	case SQLITE_REINDEX: /* Index Name    | NULL            */
		break;

	case SQLITE_ANALYZE: /* Table Name    | NULL            */
		break;

	case SQLITE_CREATE_VTABLE: /* Table Name    | Module Name     */
		break;

	case SQLITE_DROP_VTABLE: /* Table Name    | Module Name     */
		if (!checkAccess(pdb, dbname, arg1, NULL, SELINUX_DB_TABLE,
		SELINUX_DROP)) {
			rc = SQLITE_DENY;
		}

		rc = checkAllColumns(pdb, dbname, arg1, SELINUX_DB_COLUMN,
		SELINUX_DROP);

		break;

	case SQLITE_FUNCTION: /* NULL          | Function Name   */
		break;

	case SQLITE_SAVEPOINT: /* Operation     | Savepoint Name  */
		break;

	case SQLITE_COPY: /*       - No longer used -        */
		break;

	default:
		break;
	}

#ifdef SQLITE_DEBUG
	printf("\n");
#endif

	return rc;
}

/* function to insert a new_node in a list. Note that this
 function expects a pointer to head_ref as this can modify the
 head of the input linked list (similar to push())*/
void sortedInsert(struct sesqlite_context_element** head_ref,
		struct sesqlite_context_element* new_node) {
	struct sesqlite_context_element* current;

	/* Special case for the head end */
	if (*head_ref == NULL
			|| strcasecmp((*head_ref)->origin, new_node->origin) <= 0) {
		new_node->next = *head_ref;
		*head_ref = new_node;
	} else {
		/* Locate the node before the point of insertion */
		current = *head_ref;
		while (current->next != NULL
				&& strcasecmp(current->next->origin, new_node->origin) > 0) {
			current = current->next;
		}
		new_node->next = current->next;
		current->next = new_node;
	}
}

/**
 *
 */
int initializeContext(sqlite3 *db) {
	int rc = SQLITE_OK;
	int n_line, ndb_line, ntable_line, ncolumn_line, ntuple_line;
	char line[255];
	char *p, *token, *stoken;
	FILE* fp = NULL;

	//TODO modify the liselinux in order to retrieve the context from the
	//targeted folder
	fp = fopen("./sesqlite_contexts", "rb");
	if (fp == NULL) {
		fprintf(stderr, "Error. Unable to open '%s' configuration file.\n",
				"sesqlite_contexts");
		return SQLITE_OK;
	}

	sesqlite_contexts = sqlite3_malloc(sizeof(struct sesqlite_context));
	sesqlite_contexts->db_context = NULL;
	sesqlite_contexts->table_context = NULL;
	sesqlite_contexts->column_context = NULL;
	sesqlite_contexts->tuple_context = NULL;

	n_line = 0;
	ndb_line = 0;
	ntable_line = 0;
	ncolumn_line = 0;
	ntuple_line = 0;
//	while (fgets(line, sizeof line - 1, fp)) {
//		p = line;
//		while (isspace(*p))
//			p++;
//		if (*p == '#' || *p == 0)
//			continue;
//
//		token = strtok(p, " \t");
//		if (!strcasecmp(token, "db_database"))
//			ndb_line++;
//		else if (!strcasecmp(token, "db_table"))
//			ntable_line++;
//		else if (!strcasecmp(token, "db_column"))
//			ncolumn_line++;
//		else if (!strcasecmp(token, "db_tuple"))
//			ntuple_line++;
//		else {
//			fprintf(stderr,
//					"Error, unable to recognize '%s' in sesqlite_context file.\n",
//					token);
//		}
//		n_line++;
//	}
//	rewind(fp);

	while (fgets(line, sizeof line - 1, fp)) {
		if (line[strlen(line) - 1] == '\n')
			line[strlen(line) - 1] = 0;
		p = line;
		while (isspace(*p))
			p++;
		if (*p == '#' || *p == 0)
			continue;

		token = strtok(p, " \t");
		if (!strcasecmp(token, "db_database")) {
			struct sesqlite_context_element *new;
			new = sqlite3_malloc(sizeof(struct sesqlite_context_element));
			new->next = NULL;

			token = strtok(NULL, " \t");
			new->origin = strdup(token);
			char *con = strtok(NULL, " \t");
			new->security_context = strdup(con);
			new->fparam = strdup(token);
			new->sparam = strdup(token);
			new->tparam = strdup(token);

			sortedInsert(&sesqlite_contexts->db_context, new);
			ndb_line++;

		} else if (!strcasecmp(token, "db_table")) {

			struct sesqlite_context_element *new;
			new = sqlite3_malloc(sizeof(struct sesqlite_context_element));
			new->next = NULL;

			token = strtok(NULL, " \t");
			new->origin = strdup(token);
			char *con = strtok(NULL, " \t");
			new->security_context = strdup(con);
			stoken = strtok(token, ".");
			new->fparam = strdup(stoken);
			stoken = strtok(NULL, ".");
			new->sparam = strdup(stoken);
			new->tparam = NULL;

			sortedInsert(&sesqlite_contexts->table_context, new);
			ntable_line++;

		} else if (!strcasecmp(token, "db_column")) {

			struct sesqlite_context_element *new;
			new = sqlite3_malloc(sizeof(struct sesqlite_context_element));
			new->next = NULL;

			token = strtok(NULL, " \t");
			new->origin = strdup(token);
			char *con = strtok(NULL, " \t");
			new->security_context = strdup(con);
			stoken = strtok(token, ".");
			new->fparam = strdup(stoken);
			stoken = strtok(NULL, ".");
			new->sparam = strdup(stoken);
			stoken = strtok(NULL, ".");
			new->tparam = strdup(stoken);

			sortedInsert(&sesqlite_contexts->column_context, new);
			ncolumn_line++;

		} else if (!strcasecmp(token, "db_tuple")) {

			struct sesqlite_context_element *new;
			new = sqlite3_malloc(sizeof(struct sesqlite_context_element));
			new->next = NULL;

			token = strtok(NULL, " \t");
			new->origin = strdup(token);
			char *con = strtok(NULL, " \t");
			new->security_context = strdup(con);
			stoken = strtok(token, ".");
			new->fparam = strdup(stoken);
			stoken = strtok(NULL, ".");
			new->sparam = strdup(stoken);
			new->tparam = NULL;

			sortedInsert(&sesqlite_contexts->tuple_context, new);
			ntuple_line++;

		} else {
			fprintf(stderr,
					"Error, unable to recognize '%s' in sesqlite_context file.\n",
					token);
		}
	}

	fclose(fp);

//	fprintf(stdout, "\n******DATABASE\n");
//	struct sesqlite_context_element *pp;
//	pp = sesqlite_contexts->db_context;
//	while (pp != NULL) {
//		fprintf(stdout, "Origin: %s\n", pp->origin);
//		fprintf(stdout, "Fparam: %s\n", pp->fparam);
//		fprintf(stdout, "Sparam: %s\n", pp->sparam);
//		fprintf(stdout, "Tparam: %s\n", pp->tparam);
//		fprintf(stdout, "SecCon: %s\n", pp->security_context);
//		pp = pp->next;
//	}
//
//	fprintf(stdout, "\n******TABLE\n");
//	pp = NULL;
//	pp = sesqlite_contexts->table_context;
//	while (pp != NULL) {
//		fprintf(stdout, "Origin: %s\n", pp->origin);
//		fprintf(stdout, "Fparam: %s\n", pp->fparam);
//		fprintf(stdout, "Sparam: %s\n", pp->sparam);
//		fprintf(stdout, "Tparam: %s\n", pp->tparam);
//		fprintf(stdout, "SecCon: %s\n", pp->security_context);
//		pp = pp->next;
//	}
//
//	fprintf(stdout, "\n******COLUMN\n");
//	pp = NULL;
//	pp = sesqlite_contexts->column_context;
//	while (pp != NULL) {
//		fprintf(stdout, "Origin: %s\n", pp->origin);
//		fprintf(stdout, "Fparam: %s\n", pp->fparam);
//		fprintf(stdout, "Sparam: %s\n", pp->sparam);
//		fprintf(stdout, "Tparam: %s\n", pp->tparam);
//		fprintf(stdout, "SecCon: %s\n", pp->security_context);
//		pp = pp->next;
//	}
//
//	fprintf(stdout, "\n******TUPLE\n");
//	pp = NULL;
//	pp = sesqlite_contexts->tuple_context;
//	while (pp != NULL) {
//		fprintf(stdout, "Origin: %s\n", pp->origin);
//		fprintf(stdout, "Fparam: %s\n", pp->fparam);
//		fprintf(stdout, "Sparam: %s\n", pp->sparam);
//		fprintf(stdout, "Tparam: %s\n", pp->tparam);
//		fprintf(stdout, "SecCon: %s\n", pp->security_context);
//		pp = pp->next;
//	}

	/**
	 * get the tables and respectively columns in all databases.
	 */
	int i = -1; /* Database number */
	int j;
	Hash *pTbls;
	HashElem * x;
	Db *pDb;
	char *result = NULL;
	for (i = (db->nDb - 1), pDb = &db->aDb[i]; i >= 0; i--, pDb--) {
	    if (!OMIT_TEMPDB || i != 1) {
		pTbls = &db->aDb[i].pSchema->tblHash;
		for (x = sqliteHashFirst(pTbls); x; x = sqliteHashNext(x)) {
		    Table *pTab = sqliteHashData(x);
		    computeTableContext(db, pDb->zName, pTab->zName,
			sesqlite_contexts->table_context, &result);

		    if (result != NULL) {
			int id = insertId(db, result);
			sqlite3_bind_int(sesqlite_stmt, 1, id);
			sqlite3_bind_text(sesqlite_stmt, 2, pDb->zName,
					strlen(pDb->zName), SQLITE_TRANSIENT);
			sqlite3_bind_text(sesqlite_stmt, 3, pTab->zName,
					strlen(pTab->zName),
					SQLITE_TRANSIENT);
			sqlite3_bind_text(sesqlite_stmt, 4, "", strlen(""),
			SQLITE_TRANSIENT);

			rc = sqlite3_step(sesqlite_stmt);
			rc = sqlite3_reset(sesqlite_stmt);

			rc = insertKey(db, pDb->zName, pTab->zName, NULL, result);
			sqlite3_free(result);
		    }

		    if (pTab) {
			Column *pCol;
			for (j = 0, pCol = pTab->aCol; j < pTab->nCol; j++, pCol++) {
			    computeColumnContext(db, pDb->zName, pTab->zName,
				pCol->zName, sesqlite_contexts->column_context, &result);
			    if (result != NULL) {
				int id = insertId(db, result);
				sqlite3_bind_int(sesqlite_stmt, 1, id);
				sqlite3_bind_text(sesqlite_stmt, 2, pDb->zName,
						strlen(pDb->zName), SQLITE_TRANSIENT);
				sqlite3_bind_text(sesqlite_stmt, 3, pTab->zName,
						strlen(pTab->zName), SQLITE_TRANSIENT);
				sqlite3_bind_text(sesqlite_stmt, 4, pCol->zName,
						strlen(pCol->zName), SQLITE_TRANSIENT);

				rc = sqlite3_step(sesqlite_stmt);
				rc = sqlite3_reset(sesqlite_stmt);

				rc = insertKey(db, pDb->zName, pTab->zName, pCol->zName,
						result);
				sqlite3_free(result);
			    }
			}
			/* assign security context to rowid */
			computeColumnContext(db, pDb->zName, pTab->zName,
			    "ROWID", sesqlite_contexts->column_context, &result);
			if (result != NULL) {
			    int id = insertId(db, result);
			    sqlite3_bind_int(sesqlite_stmt, 1, id);
			    sqlite3_bind_text(sesqlite_stmt, 2, pDb->zName,
					    strlen(pDb->zName), SQLITE_TRANSIENT);
			    sqlite3_bind_text(sesqlite_stmt, 3, pTab->zName,
					    strlen(pTab->zName), SQLITE_TRANSIENT);
			    sqlite3_bind_text(sesqlite_stmt, 4, "ROWID",
					    strlen("ROWID"), SQLITE_TRANSIENT);

			    rc = sqlite3_step(sesqlite_stmt);
			    rc = sqlite3_reset(sesqlite_stmt);

			    rc = insertKey(db, pDb->zName, pTab->zName, "ROWID",
					    result);
			    sqlite3_free(result);
			}
		    }
		}
	    }
	}

	//free all
	struct sesqlite_context_element *pp, *pn;
	pp = sesqlite_contexts->db_context;
	while (pp != NULL) {
	    free(pp->fparam);
	    free(pp->sparam);
	    free(pp->tparam);
	    free(pp->security_context);
	    free(pp->origin);
	    pn = pp->next;
	    free(pp);
	    pp = pn;
	}

	pp = NULL;
	pp = sesqlite_contexts->table_context;
	while (pp != NULL) {
	    free(pp->fparam);
	    free(pp->sparam);
	    free(pp->tparam);
	    free(pp->security_context);
	    free(pp->origin);
	    pn = pp->next;
	    free(pp);
	    pp = pn;
	}

	pp = NULL;
	pp = sesqlite_contexts->column_context;
	while (pp != NULL) {
	    free(pp->fparam);
	    free(pp->sparam);
	    free(pp->tparam);
	    free(pp->security_context);
	    free(pp->origin);
	    pn = pp->next;
	    free(pp);
	    pp = pn;
	}

	pp = NULL;
	pp = sesqlite_contexts->tuple_context;
	while (pp != NULL) {
	    free(pp->fparam);
	    free(pp->sparam);
	    free(pp->tparam);
	    free(pp->security_context);
	    free(pp->origin);
	    pn = pp->next;
	    free(pp);
	    pp = pn;
	}

	sqlite3_free(sesqlite_contexts);
	return rc;

}

/**
 *
 */
int computeTableContext(sqlite3 *db, char *dbName, char *tblName,
		struct sesqlite_context_element * ttcon, char **res) {

    int rc = SQLITE_OK;
    int found = 0;
    struct sesqlite_context_element *p = ttcon;
    while (found == 0 && p != NULL) {
	if (strcasecmp(dbName, p->fparam) == 0 || strcmp(p->fparam, "*") == 0) {
	    if (strcasecmp(tblName, p->sparam) == 0
		    || strcmp(p->sparam, "*") == 0) {
		*res = sqlite3_mprintf(p->security_context);
		found = 1;
#ifdef SQLITE_DEBUG
fprintf(stdout, "Database: %s, Table: %s, Context found: %s\n",
    dbName, tblName, p->security_context);
#endif
	    }
	}
	p = p->next;
    }
    return rc;
}

/**
 *
 */
int computeColumnContext(sqlite3 *db, char *dbName, char *tblName,
		char *colName, struct sesqlite_context_element * ccon, char **res) {

    int rc = SQLITE_OK;
    int found = 0;
    struct sesqlite_context_element *p = ccon;
    while (found == 0 && p != NULL) {
	if (strcasecmp(dbName, p->fparam) == 0 || strcmp(p->fparam, "*") == 0) {
	    if (strcasecmp(tblName, p->sparam) == 0
		    || strcmp(p->sparam, "*") == 0) {
		if (strcasecmp(colName, p->tparam) == 0
			|| strcmp(p->tparam, "*") == 0) {
		    *res = sqlite3_mprintf(p->security_context);
		    found = 1;
#ifdef SQLITE_DEBUG
fprintf(stdout, "Database: %s, Table: %s, Column: %s, Context found: %s\n",
    dbName, tblName, colName, p->security_context);
#endif
		}
	    }
	}
	p = p->next;
    }
    return rc;
}


/**
 *
 */
int insertKey(sqlite3 *db, char *dbName, char *tName, char *cName, char *con) {
    char *key = 0;
    int id = 0;
    int *value = 0;

    if (cName == NULL)
	key = sqlite3_mprintf("%s:%s", dbName, tName);
    else
	key = sqlite3_mprintf("%s:%s:%s", dbName, tName, cName);

    char *res = 0;

    id = insertId(db, con);
    value =sqlite3_malloc(sizeof(int));
    *value = id;

    seSQLiteHashInsert(&hash, key, strlen(key), value, 0, 0);

    // DO NOT FREE key
    return SQLITE_OK;
}

/*
 * Function invoked when using the SQL function selinux_check_access
 */
static void selinuxCheckAccessFunction(sqlite3_context *context, int argc,
		sqlite3_value **argv) {

    int *res;
    int id = 0;
    id = sqlite3_value_int(argv[0]);
    assert(id != 0);

#ifdef USE_AVC
    int tclass = 0;
    int tperm = 0;
    
    int i = 0, j = 0;
    for(i = 0; i <= SELINUX_NELEM_CLASS; i++){
	if(strcmp(access_vector[i].c_name, argv[1]->z) == 0){
	    for(j = 0; j <= SELINUX_NELEM_PERM; j++){
		if(strcmp(access_vector[i].perm[j].p_name, argv[2]->z) == 0){
		    tclass = access_vector[i].c_code;
		    tperm = access_vector[i].perm[j].p_code;
		    break;
		 }
	     }
	}
    }

    unsigned int key = compress(scon_id,
	id,
	tclass,
	tperm);

    res = seSQLiteHashFind(&avc, NULL, key);
    if (res == NULL) { 
	res = sqlite3_malloc(sizeof(int));
    	char *ttcon = seSQLiteHashFind(&hash_id, NULL, id);
	*res = selinux_check_access(scon, /* source security context */
	    ttcon, /* target security context */
	    argv[1]->z, /* target security class string */
	    argv[2]->z, /* requested permissions string */
	    NULL /* auxiliary audit data */
	    );
	seSQLiteHashInsert(&avc, NULL, key, res, 0, 0);
    }
#else
    *res = selinux_check_access(scon, /* source security context */
	argv[0]->z, /* target security context */
	argv[1]->z, /* target security class string */
	argv[2]->z, /* requested permissions string */
	NULL /* auxiliary audit data */
    );
#endif

#ifdef SQLITE_DEBUG
    fprintf(stdout, "selinux_check_access(%s, %s, %s, %s) => %d\n", scon, argv[0]->z,
	argv[1]->z,
	argv[2]->z, *res);
#endif

    sqlite3_result_int(context, 0 == *res);
}

static void selinuxGetContextFunction(sqlite3_context *context, int argc,
		sqlite3_value **argv) {

    sqlite3_result_int(context, tcon_id);
}


int prepareSeSQLiteStmt(sqlite3 *db){

    int rc = SQLITE_OK;
    rc = sqlite3_exec(db, SELINUX_CONTEXT_TABLE, 0, 0, 0);
    if (rc == SQLITE_OK)
	rc = sqlite3_exec(db, SELINUX_ID_TABLE, 0, 0, 0);

    /* prepare statements */
    if (rc == SQLITE_OK)
	rc = sqlite3_prepare_v2(db,
	    "SELECT security_label from selinux_id where rowid=(?1);", -1,
	    &sesqlite_stmt_id_select, 0);

    if (rc == SQLITE_OK)
	rc = sqlite3_prepare_v2(db,
	    "SELECT rowid from selinux_id where security_label=(?1);", -1,
	    &sesqlite_stmt_con_select, 0);

    if (rc == SQLITE_OK)
	rc = sqlite3_prepare_v2(db,
	    "INSERT INTO selinux_id(security_label) values(?1);", -1,
	    &sesqlite_stmt_id_insert, 0);

    if (rc == SQLITE_OK)
	rc = sqlite3_prepare_v2(db,
	    "INSERT INTO selinux_context(security_label, db, name, column) values(?1, ?2, ?3, ?4);", -1,
	    &sesqlite_stmt, 0);


    return rc;
}


/*
 * Inizialize the database objects used by SeSqlite:
 * 1. SeSqlite master table that keeps the permission for the schema level
 * 2. Trigger to delete unused SELinux contexts after a drop table statement
 * 3. Trigger to update SELinux contexts after table rename
 */
int initializeSeSqliteObjects(sqlite3 *db) {
    int rc = SQLITE_OK;
    char *pzErr;

#ifdef SQLITE_DEBUG
fprintf(stdout, "\n == SeSqlite Initialization == \n");
#endif

//**************
    setHashMap(&hash);

    // the last 0 is to avoid copying the key inside the hash structure
    // REMEMBER TO USE MALLOC ON ALL KEYS AND NOT DEALLOCATE THEM!
    seSQLiteHashInit(&hash, SESQLITE_HASH_STRING, 0); /* init */
    seSQLiteHashInit(&avc, SESQLITE_HASH_INT, 0); /* init avc */
    seSQLiteHashInit(&hash_id, SESQLITE_HASH_INT, 0); /* init mapping */
    /* register module */
    rc = sqlite3_create_module(db, "selinuxModule", &sesqlite_mod, NULL);
    if (rc != SQLITE_OK)
	return rc;

#ifdef SQLITE_DEBUG
if (rc == SQLITE_OK)
    fprintf(stdout, "Module 'selinuxModule' registered successfully.\n");
else
    fprintf(stderr, "Error: unable to register 'sesqliteModule' module.\n");
#endif

// 	TODO attached databases could not have the triggers an the table, we should
//      consider adding an hook for the attach or the open database and
//	move the table and trigger creation there.
    if (rc == SQLITE_OK) {
	rc = prepareSeSQLiteStmt(db);

#ifdef SELINUX_STATIC_CONTEXT
    scon = sqlite3_mprintf("%s", "unconfined_u:unconfined_r:unconfined_t:s0");
    tcon = sqlite3_mprintf("%s", "unconfined_u:object_r:unconfined_t:s0");
#else
    rc = getcon(&scon);
    if(security_compute_create_raw(scon, scon, 4, &tcon) < 0){
	fprintf(stderr, "SELinux could not compute a default context\n");
	return 0;
    }
#endif

    scon_id = insertId(db, scon);
    tcon_id = insertId(db, tcon);

    if (rc == SQLITE_OK)
	rc = initializeContext(db);

#ifdef SQLITE_DEBUG
if (rc == SQLITE_OK)
    fprintf(stdout, "Virtual table 'sesqlite_master' created successfully.\n");
else
    fprintf(stderr, "Error: unable to create VirtualTable 'sesqlite_master'.\n");
#endif
    }

#ifdef SQLITE_DEBUG
if (rc != SQLITE_OK)
    fprintf(stderr, "Error: unable to initialize the selinux support for SQLite.\n");
#endif

    return rc;
}

int create_security_context_column(void *pUserData, void *parse, int type, void *pNew, 
	char **zColumn) {

    sqlite3* db = pUserData;
    Parse *pParse = parse;
    Column *pCol;
    char *zName = 0;
    char *zType = 0;
    int op = 0;
    int nExtra = 0;
    Expr *pExpr;
    int c = 0;
    int i = 0;
    int iDb = 0;

    *zColumn = 0;
    *zColumn = sqlite3MPrintf(db, SECURITY_CONTEXT_COLUMN_DEFINITION);
    sqlite3Dequote(*zColumn);

    Table *p = pNew;
    iDb = sqlite3SchemaToIndex(db, p->pSchema);
#if SQLITE_MAX_COLUMN
  if( p->nCol+1>db->aLimit[SQLITE_LIMIT_COLUMN] ){
    //sqlite3ErrorMsg(pParse, "too many columns on %s", p->zName);
    return;
  }
#endif
    zName = sqlite3MPrintf(db, SECURITY_CONTEXT_COLUMN_NAME);
    sqlite3Dequote(zName);
    for(i=0; i<p->nCol; i++){
	if( STRICMP(zName, p->aCol[i].zName) ){
      //sqlite3ErrorMsg(pParse, "object name reserved for internal use: %s", zName);
	    sqlite3DbFree(db, zName);
	    sqlite3DbFree(db, *zColumn);
	    return;
	}
    }

    if( (p->nCol & 0x7)==0 ){
	Column *aNew;
	aNew = sqlite3DbRealloc(db,p->aCol,(p->nCol+8)*sizeof(p->aCol[0]));
	if( aNew==0 ){
	    //sqlite3ErrorMsg(pParse, "memory error");
	    sqlite3DbFree(db, zName);
	    sqlite3DbFree(db, *zColumn);
	return;
	}
	p->aCol = aNew;
    }
    pCol = &p->aCol[p->nCol];
    memset(pCol, 0, sizeof(p->aCol[0]));
    pCol->zName = zName;

    zType = sqlite3MPrintf(db, SECURITY_CONTEXT_COLUMN_TYPE);
    sqlite3Dequote(zType);
    pCol->zType = sqlite3MPrintf(db, zType);
    pCol->affinity = SQLITE_AFF_INTEGER;
    p->nCol++;
    p->aCol[p->nCol].colFlags |= COLFLAG_HIDDEN;

    /**
    *generate expression for DEFAULT value
    */
    op = 151;
    nExtra = 7;
    pExpr = sqlite3DbMallocZero(db, sizeof(Expr)+nExtra);
    pExpr->op = (u8)op;
    pExpr->iAgg = -1;
    pExpr->u.zToken = (char*)&pExpr[1];
    memcpy(pExpr->u.zToken, SECURITY_CONTEXT_COLUMN_DEFAULT_FUNC, strlen(SECURITY_CONTEXT_COLUMN_DEFAULT_FUNC) - 2);
    pExpr->u.zToken[strlen(SECURITY_CONTEXT_COLUMN_DEFAULT_FUNC) - 2] = 0;
    sqlite3Dequote(pExpr->u.zToken);
    pExpr->flags |= EP_DblQuoted;
#if SQLITE_MAX_EXPR_DEPTH>0
    pExpr->nHeight = 1;
#endif 
    pCol->pDflt = pExpr;
    pCol->zDflt = sqlite3DbStrNDup(db, SECURITY_CONTEXT_COLUMN_DEFAULT_FUNC
	      , strlen(SECURITY_CONTEXT_COLUMN_DEFAULT_FUNC));
      
      
    /* Loop through the columns of the table to see if any of them contain the token "hidden".
     ** If so, set the Column.isHidden flag and remove the token from
     ** the type string.  */
    int iCol;
    for (iCol = 0; iCol < p->nCol; iCol++) {
	char *zType = p->aCol[iCol].zType;
	char *zName = p->aCol[iCol].zName;
	int nType;
	int i = 0;
	if (!zType)
	continue;
	nType = sqlite3Strlen30(zType);
	if ( sqlite3StrNICmp("hidden", zType, 6)
			|| (zType[6] && zType[6] != ' ')) {
	    for (i = 0; i < nType; i++) {
		if ((0 == sqlite3StrNICmp(" hidden", &zType[i], 7))
				&& (zType[i + 7] == '\0' || zType[i + 7] == ' ')) {
		    i++;
		    break;
		}
	    }
	}
	if (i < nType) {
	    int j;
	    int nDel = 6 + (zType[i + 6] ? 1 : 0);
	    for (j = i; (j + nDel) <= nType; j++) {
		    zType[j] = zType[j + nDel];
	    }
	    if (zType[i] == '\0' && i > 0) {
		    assert(zType[i-1]==' ');
		    zType[i - 1] = '\0';
	    }
            p->aCol[iCol].colFlags |= COLFLAG_HIDDEN;
	}
    }
    //assign security context to sql schema object
    //insert table context
    sqlite3NestedParse(pParse,
      "INSERT INTO %Q.%s (security_label, db, name) VALUES(getcon(), '%s', '%s')",
      pParse->db->aDb[iDb].zName, "selinux_context",
      pParse->db->aDb[iDb].zName, p->zName
    );
    sqlite3ChangeCookie(pParse, iDb);

    //add security context to columns
    for (iCol = 0; iCol < p->nCol; iCol++) {
    	sqlite3NestedParse(pParse,
    	  "INSERT INTO %Q.%s(security_label, db, name, column) VALUES(getcon(), '%s', '%s', '%s')",
    	  pParse->db->aDb[iDb].zName, "selinux_context",
    	  pParse->db->aDb[iDb].zName, p->zName, p->aCol[iCol].zName
    	);
    }
    sqlite3ChangeCookie(pParse, iDb);

    sqlite3NestedParse(pParse,
      "INSERT INTO %Q.%s(security_label, db, name, column) VALUES(getcon(), '%s', '%s', '%s')",
      pParse->db->aDb[iDb].zName, "selinux_context",
      pParse->db->aDb[iDb].zName, p->zName, "ROWID" 
    );
    sqlite3ChangeCookie(pParse, iDb);

    return SQLITE_OK;
}

static int selinux_schemachange_callback(
  void* pArg,                  /* the user argument (the db in our case) */
  int op,                      /* the schema operation that triggered the callback */
  const char* zDb,             /* the name of the database */
  const char* zTable,          /* the name of the table */
  void* arg1,                  /* arg1 depends on the op */
  void* arg2                   /* arg2 depends on the op */
){
  sqlite3 *db = (sqlite3*) pArg;
  Parse *pParse = sqlite3_next_stmt(db, 0);

  switch( op ){

  case SQLITE_SCHEMA_CREATE_TABLE:
    break;

  case SQLITE_SCHEMA_DROP_TABLE:
    sqlite3NestedParse(pParse,
      "DELETE FROM %s.%s WHERE db = %s AND name = %s",
      zDb, "selinux_context", zDb, zTable);
    break;

  case SQLITE_SCHEMA_ALTER_RENAME:
    sqlite3NestedParse(pParse,
      "UPDATE %s.%s SET name = %s WHERE db = %s AND name = %s",
      zDb, "selinux_context", zTable, zDb, arg1);
    break;

  case SQLITE_SCHEMA_ALTER_ADD:
    break;

  }

  return 0;
}

/*
 * Function: sqlite3SelinuxInit
 * Purpose: Initialize SeSqlite and register objects, authorizer and functions.
 * 			This function is called by the SQLite core in case the SQLITE_CORE
 * 			compile flag has been enabled or at runtime when the extension is loaded.
 * Parameters:
 * 				sqlite3 *db: a pointer to the SQLite database.
 * Return value: 0->OK, other->ERROR (see **pzErr for info about error)
 */
int sqlite3SelinuxInit(sqlite3 *db) {

    int rc = 0;

    /* create the SQL function getcon */
    if (rc == SQLITE_OK)
	rc = sqlite3_create_function(db, "getcon", 0,
	    SQLITE_UTF8 /* | SQLITE_DETERMINISTIC */, 0, selinuxGetContextFunction,
	    0, 0);

    /* create the SQL function selinux_check_access */
    if (rc == SQLITE_OK)
	rc = sqlite3_create_function(db, "selinux_check_access", 3,
	    SQLITE_UTF8 /* | SQLITE_DETERMINISTIC */, 0, selinuxCheckAccessFunction,
	    0, 0);

    /* initialize SeSQLite objects */
    if (rc == SQLITE_OK)
	rc = initializeSeSqliteObjects(db);

    /* set the extra_column_callback */
    if(rc == SQLITE_OK)
	rc =sqlite3_set_add_extra_column(db, create_security_context_column, db);

    /* set the schemachange_callback */
    if( rc == SQLITE_OK )
	rc = sqlite3_schemachange_hook(db, selinux_schemachange_callback, db);

    /* set the authorizer_authorizer */
    if (rc == SQLITE_OK)
	rc = sqlite3_set_authorizer(db, selinuxAuthorizer, db);

    return rc;
}


/* Runtime-loading extension support */

#if !SQLITE_CORE
int sqlite3_extension_init(sqlite3 *db, char **pzErrMsg,
		const sqlite3_api_routines *pApi) {
	SQLITE_EXTENSION_INIT2(pApi);
	return sqlite3SelinuxInit(db);
}
#endif

#endif /* !defined(SQLITE_CORE) || defined(SQLITE_ENABLE_SELINUX) */
