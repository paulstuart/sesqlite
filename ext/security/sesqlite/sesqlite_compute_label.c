

#include "sesqlite.h"

/**
 *
 */
int compute_sql_context(int isColumn, char *dbName, char *tblName,
	char *colName, struct sesqlite_context_element * con, char **res) {

    int rc = SQLITE_OK;
    int found = 0;
    struct sesqlite_context_element *p = 0;
    p = con;
    while (found == 0 && p != NULL) {
	if (strcasecmp(dbName, p->fparam) == 0 || strcmp(p->fparam, "*") == 0) {
	    if (strcasecmp(tblName, p->sparam) == 0
		    || strcmp(p->sparam, "*") == 0) {
		if(isColumn){
		    if (strcasecmp(colName, p->tparam) == 0
			    || strcmp(p->tparam, "*") == 0)
			break;	
		}else
		    break; 
	    }
	}
	p = p->next;
    }
    if(p != NULL)
	*res = sqlite3_mprintf("%s", p->security_context);
    else{
	/* the sesqlite_context file does not contain a context for the
	 * table/column we want to store, then compute the default one. */
	security_context_t p_con = NULL;
	rc = getcon(&p_con);
	if(security_compute_create_raw(p_con, p_con, 4, res) < 0){
	    fprintf(stderr, "SELinux could not compute a default context\n");
	    return 0;
	}
    }
    return rc;
}


int lookup_security_context(seSQLiteBiHash *hash, char *db_name, char *tbl_name){

    int *id = NULL;
    char *sec_context = NULL;

    compute_sql_context(0, db_name, tbl_name, NULL, 
	    sesqlite_contexts->tuple_context, &sec_context);

    id = seSQLiteBiHashFindKey(hash, sec_context, strlen(sec_context));
    assert(id != NULL); /* check if SELinux can compute a security context */

    sqlite3_free(sec_context);
    return *(int *) id;
}

int lookup_security_label(seSQLiteBiHash *hash, int type, char *db_name, char *tbl_name, char *col_name){

    int *id = NULL;
    char *sec_context = NULL;

    if(type){
	compute_sql_context(0, db_name, tbl_name, col_name, 
	    sesqlite_contexts->column_context, &sec_context);

    }else{
	compute_sql_context(0, db_name, tbl_name, NULL, 
	    sesqlite_contexts->table_context, &sec_context);
    }

    assert(sec_context != NULL);
    id = seSQLiteBiHashFindKey(hash, sec_context, strlen(sec_context));
    assert(id != NULL); /* check if SELinux can compute a security context */

    sqlite3_free(sec_context);
    return *(int *) id;
}


//static void selinuxGetSecurityContextFunction(sqlite3_context *context, int argc,
//		sqlite3_value **argv) {
//
//    assert(argc == 2);
//
//    int *id = NULL;
//    char *sec_context = NULL;
//    const char *db_name = sqlite3_value_text(argv[0]);
//    const char *tbl_name = sqlite3_value_text(argv[1]);
//
//    compute_sql_context(0, (char *) db_name, (char *) tbl_name, NULL, 
//	    sesqlite_contexts->tuple_context, &sec_context);
//
//    id = seSQLiteBiHashFindKey(hash_id, sec_context, strlen(sec_context));
//    assert(id != NULL); /* check if SELinux can compute a security context */
//
//    sqlite3_free(sec_context);
//    sqlite3_result_int(context, *(int *) id);
//}
//
//static void selinuxGetSecurityLabelFunction(sqlite3_context *context, int argc,
//		sqlite3_value **argv) {
//
//    assert(argc == 4);
//
//    int *id = NULL;
//    char *sec_context = NULL;
//    int type = sqlite3_value_int(argv[0]);
//    const char *db_name = sqlite3_value_text(argv[1]);
//    const char *tbl_name = sqlite3_value_text(argv[2]);
//    const char *col_name = sqlite3_value_text(argv[3]);
//
//    if(type){
//	compute_sql_context(0, (char *) db_name, (char *) tbl_name, (char *) col_name, 
//	    sesqlite_contexts->column_context, &sec_context);
//
//    }else{
//	compute_sql_context(0, (char *) db_name, (char *) tbl_name, NULL, 
//	    sesqlite_contexts->table_context, &sec_context);
//    }
//
//
//    assert(sec_context != NULL);
//    id = seSQLiteBiHashFindKey(hash_id, sec_context, strlen(sec_context));
//    assert(id != NULL); /* check if SELinux can compute a security context */
//
//    sqlite3_free(sec_context);
//    sqlite3_result_int(context, *(int *) id);
//}


