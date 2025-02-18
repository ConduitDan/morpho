/** @file sparse.c
 *  @author T J Atherton
 *
 *  @brief Veneer class over the objectsparse type that provides sparse matrices
 */

#include "build.h"
#include "sparse.h"
#include "morpho.h"
#include "dictionary.h"
#include "common.h"
#include "matrix.h"
#include "builtin.h"
#include "veneer.h"

#include <limits.h>
#include <stdlib.h>

/* ***************************************
 * Compatibility with Sparse libraries
 * *************************************** */

/** ---- CSparse --- */
#ifdef MORPHO_LINALG_USE_CSPARSE
#include <cs.h>

/* Convert a CCS structure into a CSPARSE structure */
void sparse_ccstocsparse(sparseccs *s, cs *out) {
    out->nzmax=s->nentries;
    out->m=s->nrows;
    out->n=s->ncols;
    out->p=s->cptr;
    out->i=s->rix;
    out->x=s->values;
    out->nz=-1;
}

/* Convert a CCS structure into a CSPARSE structure */
void sparse_csparsetoccs(cs *in, sparseccs *out) {
    out->nentries=in->nzmax;
    out->nrows=in->m;
    out->ncols=in->n;
    out->cptr=in->p;
    out->rix=in->i;
    out->values=in->x;
}
#endif

/* ***************************************
 * Dictionary of keys format
 * *************************************** */

DEFINE_VARRAY(dokkey, objectdokkey);

/** Initializes a sparsedok structure */
void sparsedok_init(sparsedok *dok) {
    dok->nrows=0;
    dok->ncols=0;
    dictionary_init(&dok->dict);
    dok->keys=NULL;
}

/** Clears a sparsedok structure */
void sparsedok_clear(sparsedok *dok) {
    objectdokkey *next=NULL;
    for (objectdokkey *key=dok->keys; key!=NULL; key=next) {
        next=(objectdokkey *) key->obj.next;
        MORPHO_FREE(key);
    }
    dictionary_clear(&dok->dict);
    sparsedok_init(dok);
}

/** Create a new key from a pair of indices */
static objectdokkey *sparsedok_newkey(sparsedok *dok, int i, int j) {
    objectdokkey *key = MORPHO_MALLOC(sizeof(objectdokkey));
    
    if (key) {
        object_init((object *) key, OBJECT_DOKKEY);
        key->row=i;
        key->col=j;
    }
    return key;
}

/** Adds a key to a dok structure.
 * @returns the key added. */
static objectdokkey *sparsedok_addkey(sparsedok *dok, int i, int j) {
    objectdokkey *out=sparsedok_newkey(dok, i, j);
    
    if (out) {
        out->obj.next=(object *) dok->keys;
        dok->keys=out;
    }
        
    return out;
}

/** Inserts a matrix element (i,j) -> val into a sparsedok structure
 * @returns true on success. */
bool sparsedok_insert(sparsedok *dok, int i, int j, value val) {
    objectdokkey key=MORPHO_STATICDOKKEY(i, j);
    if (!dictionary_get(&dok->dict, MORPHO_OBJECT(&key), NULL)) {
        objectdokkey *newkey=sparsedok_addkey(dok, i, j);
        if (newkey) {
            if (dok->nrows==0 || i>=dok->nrows) dok->nrows=i+1;
            if (dok->ncols==0 || j>=dok->ncols) dok->ncols=j+1;
            return dictionary_insert(&dok->dict, MORPHO_OBJECT(newkey), val);
        }
    } else {
        return dictionary_insert(&dok->dict, MORPHO_OBJECT(&key), val);
    }
    
    return false;
}

/** Retrieves a matrix element (i,j) from a sparsedok structure
 * @returns true on success. */
bool sparsedok_get(sparsedok *dok, int i, int j, value *val) {
    objectdokkey key=MORPHO_STATICDOKKEY(i, j);
    return dictionary_get(&dok->dict, MORPHO_OBJECT(&key), val);
}

/** Removes a matrix element (i,j) from a sparsedok
 * @returns true on success.
 * @warning Use sparingly as the deleted key is not recovered. */
bool sparsedok_remove(sparsedok *dok, int i, int j, value *val) {
    objectdokkey key=MORPHO_STATICDOKKEY(i, j);
    return dictionary_remove(&dok->dict, MORPHO_OBJECT(&key));
}

/** Sets the dimensions of the matrix
 * @returns true if successful, or false if the dimensions are incompatible with existing matrix entries
 * @details This function is intended for use in constructing matrix. */
bool sparsedok_setdimensions(sparsedok *dok, int nrows, int ncols) {
    if (nrows<dok->nrows || ncols<dok->ncols) return false;
    dok->nrows=nrows;
    dok->ncols=ncols;
    return true;
}

/** Prints a sparsedok matrix */
void sparsedok_print(sparsedok *dok) {
    value out;
    for (int i=0; i<dok->nrows; i++) {
        printf("[ ");
        for (int j=0; j<dok->ncols; j++) {
            if (sparsedok_get(dok, i, j, &out)) {
                morpho_printvalue(out);
                printf(" ");
            } else {
                printf("0 ");
            }
        }
        printf("]%s", (i<dok->nrows-1 ? "\n" : ""));
    }
}

/** Number of entries in a sparsedok */
unsigned int sparsedok_count(sparsedok *dok) {
    return dok->dict.count;
}

/** Loop over dok keys - initializer
 * @param[in] dok - the dictionary of keys to loop over
 * @returns Initial value for the loop counter */
void *sparsedok_loopstart(sparsedok *dok) {
    return dok->keys;
}

/** Loop over dok keys
 * @param[in] dok - the dictionary of keys to loop over
 * @param[in] cntr - Pointer to loop counter of type (void *)
 * @param[out] i - row index.
 * @param[out] j - column index
 * @returns true if i, j contain valid data; cntri is updated */
bool sparsedok_loop(sparsedok *dok, void **cntr, int *i, int *j) {
    objectdokkey *key = *cntr;
    if (key) {
        *i = key->row;
        *j = key->col;
        *cntr=key->obj.next;
    }
    return key;
}

/* Copies a sparsedok object */
bool sparsedok_copy(sparsedok *src, sparsedok *dest) {
    int i, j;
    void *ctr = sparsedok_loopstart(src);
    value entry;
    
    if (!sparsedok_setdimensions(dest, src->nrows, src->ncols)) return false;
    
    while (sparsedok_loop(src, &ctr, &i, &j)) {
        if (sparsedok_get(src, i, j, &entry)) {
            if (!sparsedok_insert(dest, i, j, entry)) return false;
        }
    }

    return true;
}

/* ***************************************
 * Compressed Column Storage Format
 * *************************************** */

/** Initializes an empty sparseccs */
void sparseccs_init(sparseccs *ccs) {
    ccs->nentries=0;
    ccs->nrows=0;
    ccs->ncols=0;
    ccs->cptr=NULL;
    ccs->rix=NULL;
    ccs->values=NULL;
}

/** Clears all data structures associated with a sparseccs */
void sparseccs_clear(sparseccs *ccs) {
    if (ccs->cptr) MORPHO_FREE(ccs->cptr);
    if (ccs->rix) MORPHO_FREE(ccs->rix);
    if (ccs->values) MORPHO_FREE(ccs->values);
    sparseccs_init(ccs);
}

/** Resizes a sparseccs */
bool sparseccs_resize(sparseccs *ccs, int nrows, int ncols, unsigned int nentries, bool values) {
    if (ncols>ccs->ncols) {
        ccs->cptr=MORPHO_REALLOC(ccs->cptr, sizeof(int)*(ncols+1));
        if (ccs->values || values) {
            ccs->values=MORPHO_REALLOC(ccs->values, sizeof(double)*nentries);
            if (!ccs->values) goto sparseccs_resize_error; 
        }
    }
    ccs->rix=MORPHO_REALLOC(ccs->rix, sizeof(int)*nentries);
    if (!(ccs->cptr && ccs->rix)) goto sparseccs_resize_error;
    
    ccs->ncols=ncols;
    ccs->nrows=nrows;
    ccs->nentries=nentries;
    return true;
    
sparseccs_resize_error:
    sparseccs_clear(ccs);
    return false;
}

/** Retrieves the row indices given a column
 * @param[in] ccs   the matrix
 * @param[in] col  column index
 * @param[out] nentries  the number of entries
 * @param[out] entries  the entries themselves */
bool sparseccs_getrowindices(sparseccs *ccs, int col, int *nentries, int **entries) {
    if (col>=ccs->ncols) return false;
    *nentries=ccs->cptr[col+1]-ccs->cptr[col];
    *entries=ccs->rix+ccs->cptr[col];
    return true;
}

/** Sets the row indices given a column
 * @param[in] ccs   the matrix
 * @param[in] col  column index
 * @param[out] nentries  the number of entries
 * @param[out] entries  the entries themselves
 * @warning Use with caution */
bool sparseccs_setrowindices(sparseccs *ccs, int col, int nentries, int *entries) {
    if (col>=ccs->ncols) return false;
    if (nentries!=ccs->cptr[col+1]-ccs->cptr[col]) return false;
    int *e=ccs->rix+ccs->cptr[col];
    for (unsigned int i=0; i<nentries; i++) e[i]=entries[i];
    
    return true;
}

/** Retrieves the indices of non-zero columns
 * @param[in] ccs   the matrix
 * @param[in] maxentries maximum number of entries
 * @param[out] nentries  the number of entries
 * @param[out] entries  the entries themselves (call with NULL to get the size of the required array) */
bool sparseccs_getcolindices(sparseccs *ccs, int maxentries, int *nentries, int *entries) {
    int k=0;
    
    for (int i=0; i<ccs->ncols; i++) {
        if (ccs->cptr[i+1]!=ccs->cptr[i]) {
            if (entries && k<maxentries) entries[k]=i;
            k++;
        }
    }
    *nentries=k;
    
    return true;
}

/** Retrieves the indices of all columns that contain a nonzero entry on a particular row.
 * @param[in] ccs   the matrix
 * @param[in] row  the row to index
 * @param[in] maxentries maximum number of entries
 * @param[out] nentries  the number of entries
 * @param[out] entries  the entries themselves (call with NULL to get the size of the required array) */
bool sparseccs_getcolindicesforrow(sparseccs *ccs, int row, int maxentries, int *nentries,  int *entries) {
    int col=0, k=0;
    
    for (unsigned int i=0; i<ccs->nentries; i++) {
        while (ccs->cptr[col+1]<=i) col++;
        if (ccs->rix[i]==row) {
            if (entries && k<maxentries) entries[k]=col;
            k++;
        }
    }
    
    *nentries=k;
    return true;
}

/** Sets a matrix element (i,j) to be a specified value
 * @returns true if the element exists in the given sparsity structure, false otherwise. */
bool sparseccs_set(sparseccs *ccs, int i, int j, double val) {
    int k;
    for (k=ccs->cptr[j]; k<ccs->cptr[j+1]; k++) {
        if (ccs->rix[k]==i) {
            if (ccs->values) ccs->values[k]=val;
            return true;
        }
    }
    return false;
}

/** Retrieves a matrix element (i,j) from a sparseccs structure
 * @returns true on success. */
bool sparseccs_get(sparseccs *ccs, int i, int j, double *val) {
    int k;
    for (k=ccs->cptr[j]; k<ccs->cptr[j+1]; k++) {
        if (ccs->rix[k]==i) {
            if (val) *val=(ccs->values ? ccs->values[k] : 1.0);
            return true;
        }
    }
    return false;
}

/** Helper function to compare unsigned integers */
static int sparseccs_compareuint(const void * a, const void * b) {
    long int i=*(int*)a, j=*(int*)b;
    return (int) (i-j);
}

/** Converts a DOK matrix to a CCS matrix */
bool sparseccs_doktoccs(sparsedok *in, sparseccs *out, bool copyvals) {
    int nentries=in->dict.count;
    
    sparseccs_init(out);
    if (!sparseccs_resize(out, in->nrows, in->ncols, nentries, copyvals)) return false;
    
    /* Clear the column pointer array */
    for (int i=0; i<in->ncols+1; i++) out->cptr[i]=0;
        
    /* Count number of entries per column */
    for (unsigned int i=0; i<in->dict.capacity; i++) {
        value key=in->dict.contents[i].key;
        if (MORPHO_ISDOKKEY(key)) out->cptr[MORPHO_GETDOKCOLWVAL(key)]++;
    }
    
    /* Construct the column pointer array */
    unsigned int ptr=0;
    for (int i=0; i<in->ncols+1; i++) {
        int p=ptr;
        ptr+=out->cptr[i];
        out->cptr[i]=p;
    }
    
    /* Clear the row index array */
    for (int i=0; i<nentries; i++) out->rix[i]=-1;
    
    /* Copy entries into appropriate rowindex */
    for (unsigned int i=0; i<in->dict.capacity; i++) {
        value key=in->dict.contents[i].key;
        if (MORPHO_ISDOKKEY(key)) {
            int k=out->cptr[MORPHO_GETDOKCOLWVAL(key)];
            while (out->rix[k]!=-1) k++;
            out->rix[k]=MORPHO_GETDOKROWWVAL(key);
        }
    }
    
    /* Sort columns */
    for (int i=0; i<in->ncols; i++) {
        int len=out->cptr[i+1]-out->cptr[i];
        if (len>1) {
            qsort(out->rix+out->cptr[i], len, sizeof(int), sparseccs_compareuint);
        }
    }
    
    /* Copy over values */
    if (copyvals) {
        for (int i=0; i<nentries; i++) out->values[i]=0.0;
        
        for (int j=0; j<in->ncols; j++) {
            int len=out->cptr[j+1]-out->cptr[j];
            for (int i=0; i<len; i++) {
                value val;
                if (sparsedok_get(in, out->rix[out->cptr[j]+i], j, &val)) {
                    if (MORPHO_ISFLOAT(val)) out->values[out->cptr[j]+i]=MORPHO_GETFLOATVALUE(val);
                    else if (MORPHO_ISINTEGER(val)) out->values[out->cptr[j]+i]=MORPHO_GETINTEGERVALUE(val);
                }
            }
        }
    }
    
    return true;
}

/** Prints a sparsedok matrix */
void sparseccs_print(sparseccs *ccs) {
    double val;
    for (int i=0; i<ccs->nrows; i++) {
        printf("[ ");
        for (int j=0; j<ccs->ncols; j++) {
            if (sparseccs_get(ccs, i, j, &val)) printf("%g ", val);
            else printf("0 ");
        }
        printf("]%s", (i<ccs->nrows-1 ? "\n" : ""));
    }
}

/** Number of entries in a sparseccs */
unsigned int sparseccs_count(sparseccs *ccs) {
    return ccs->nentries;
}

/** Copies one sparseccs matrix to another, reallocating as necessary */
bool sparseccs_copy(sparseccs *src, sparseccs *dest) {
    bool success=false;
    if (sparseccs_resize(dest, src->nrows, src->ncols, src->nentries, src->values)) {
        memcpy(dest->cptr, src->cptr, sizeof(int)*(src->ncols+1));
        memcpy(dest->rix, src->rix, sizeof(int)*(src->nentries));
        if (src->values) memcpy(dest->values, src->values, sizeof(double)*src->nentries);
        success=true;
    }
    return success;
}

/* ***************************************
 * Object sparse interface
 * *************************************** */

/** Checks whether a format is available.
 * @param sparse  the matrix to check
 * @param format format to check
 * @param force if format is unavailable, try to make it available
 * @param copyvals copy values across
 * @returns true if the format is available */
bool sparse_checkformat(objectsparse *sparse, objectsparseformat format, bool force, bool copyvals) {
    switch (format) {
        case SPARSE_DOK:
            return (sparse->dok.ncols>0 && sparse->dok.nrows>0)||(sparse->dok.dict.count>0);
        case SPARSE_CCS:
            if (force && !sparse->ccs.cptr) {
                return sparseccs_doktoccs(&sparse->dok, &sparse->ccs, copyvals);
            } else return (sparse->ccs.cptr);
    }
}

/** Removes data structures for a given format */
void sparse_removeformat(objectsparse *s,  objectsparseformat format) {
    if (format==SPARSE_DOK) {
        sparsedok_clear(&s->dok);
    } else {
        sparseccs_clear(&s->ccs);
    }
}

/* ***************************************
 * Testing code
 * *************************************** */

void sparse_test(void) {
    sparsedok dok;
    sparseccs ccs;
    int nval=7;
    int elements[] = {0,0, 1,1, 2,2, 3,3, 1,2, 2,1, 0,3};
    value val[] = {MORPHO_INTEGER(1),MORPHO_INTEGER(2),MORPHO_INTEGER(3),MORPHO_INTEGER(4),MORPHO_INTEGER(-1),MORPHO_INTEGER(-1),MORPHO_INTEGER(5)};
    
    sparsedok_init(&dok);
    
    for (int i=0; i<nval; i++) {
        sparsedok_insert(&dok, elements[2*i], elements[2*i+1], val[i]);
    }
    
    sparsedok_print(&dok);
    
    sparseccs_doktoccs(&dok, &ccs, true);
    
    for (int i=0; i<ccs.ncols; i++) printf("%u ", ccs.cptr[i]);
    printf("\n");
    for (int i=0; i<ccs.nentries; i++) printf("(%u %g) ", ccs.rix[i], ccs.values[i]);
    printf("\n");
    
    sparseccs_print(&ccs);
    
    sparsedok_clear(&dok);
    sparseccs_clear(&ccs);
}

/* ***************************************
 * objectsparse objects
 * *************************************** */

/** Creates a sparse matrix object
 * @param[in] nrows } Optional number of rows and columns
 * @param[in] ncols } */
objectsparse *object_newsparse(int *nrows, int *ncols) {
    objectsparse *new = (objectsparse *) object_new(sizeof(objectsparse), OBJECT_SPARSE);
    
    if (new) {
        sparsedok_init(&new->dok);
        sparseccs_init(&new->ccs);
        if (nrows) sparsedok_setdimensions(&new->dok, *nrows, *ncols);
    }
    
    return new;
}

/** Create a sparse array from an array */
objectsparse *object_sparsefromarray(objectarray *array) {
    unsigned int dim[2] = {0,0}, ndim;
    
    if (!matrix_getarraydimensions(array, dim, 2, &ndim)) return NULL;
    
    objectsparse *new=object_newsparse(NULL, NULL);
    
    for (unsigned int i=0; i<dim[0]; i++) {
        value v[3]={MORPHO_NIL, MORPHO_NIL, MORPHO_NIL};
        for (unsigned int k=0; k<dim[1] && k<3; k++) {
            unsigned int indx[2] = {i, k};
            v[k]=matrix_getarrayelement(array, 2, indx);
        }
        if (MORPHO_ISINTEGER(v[0]) && MORPHO_ISINTEGER(v[1])) {
            sparsedok_insert(&new->dok, MORPHO_GETINTEGERVALUE(v[0]), MORPHO_GETINTEGERVALUE(v[1]), v[2]);
        } else {
            sparse_clear(new);
            MORPHO_FREE(new);
            return false;
        }
    }
    
    return new;
}

/** Create a sparse array from an array */
objectsparse *object_sparsefromlist(objectlist *list) {
    unsigned int dim[2] = {0,0}, ndim;
    
    if (!matrix_getlistdimensions(list, dim, 2, &ndim)) return NULL;
    
    objectsparse *new=object_newsparse(NULL, NULL);
    
    for (unsigned int i=0; i<dim[0]; i++) {
        value v[3]={MORPHO_NIL, MORPHO_NIL, MORPHO_NIL};
        for (unsigned int k=0; k<dim[1] && k<3; k++) {
            unsigned int indx[2] = {i, k};
            matrix_getlistelement(list, 2, indx, &v[k]);
        }
        if (MORPHO_ISINTEGER(v[0]) && MORPHO_ISINTEGER(v[1])) {
            sparsedok_insert(&new->dok, MORPHO_GETINTEGERVALUE(v[0]), MORPHO_GETINTEGERVALUE(v[1]), v[2]);
        } else {
            sparse_clear(new);
            MORPHO_FREE(new);
            return false;
        }
    }
    
    return new;
}

/** Clones a sparse matrix */
objectsparse *sparse_clone(objectsparse *s) {
    objectsparse *new = object_newsparse(NULL, NULL);
 
    if (new) {
        sparsedok_copy(&s->dok, &new->dok);
        sparseccs_copy(&s->ccs, &new->ccs);
    }
    
    return new;
}

/** Set an element */
bool sparse_setelement(objectsparse *s, int row, int col, value val) {
    if (sparsedok_insert(&s->dok, row, col, val)) {
        sparse_removeformat(s, SPARSE_CCS);
        return true;
    }
    /* Should invalidate CCS here */
    return false;
}

/** Get an element
 * @param[in] s the sparse object
 * @param[in] row the row
 * @param[in] col the column
 * @param[out] val the value; pass NULL to check if an element exists */
bool sparse_getelement(objectsparse *s, int row, int col, value *val) {
    if (sparse_checkformat(s, SPARSE_DOK, false, false)) {
        return sparsedok_get(&s->dok, row, col, val);
    } else if (sparse_checkformat(s, SPARSE_CCS, false, false)) {
        double v;
        if (sparseccs_get(&s->ccs, row, col, &v)) {
            if (val) *val = MORPHO_FLOAT(v);
        }
    }
    return false;
}

/** Enumerate values in a sparse matrix */
bool sparse_enumerate(objectsparse *s, int i, value *out) {
    if (sparse_checkformat(s, SPARSE_CCS, false, false)) {
        if (i<0) { *out=MORPHO_INTEGER(s->ccs.nentries); return true; }
        if (i<s->ccs.nentries) { *out=MORPHO_FLOAT(s->ccs.values[i]); return true; }
    } else if (sparse_checkformat(s, SPARSE_DOK, false, false)) {
        if (i<0) { *out=MORPHO_INTEGER(s->dok.dict.count); return true; }
        if (i<s->dok.dict.count) {
            objectdokkey *key = s->dok.keys;
            for (int k=0; k<i; k++) if (key) key=(objectdokkey *) key->obj.next;
            
            if (key) return dictionary_get(&s->dok.dict, MORPHO_OBJECT(key), out);
        }
    }
    
    return false;
}

/** Add two matrices
 * @param[in] a - sparse matrix
 * @param[in] b - sparse matrix
 * @param[in] alpha - scale for a
 * @param[in] beta - scale for b
 * @param[out] out - alpha*a+beta*b. */
objectsparseerror sparse_add(objectsparse *a, objectsparse *b, double alpha, double beta, objectsparse *out) {
    if (!(sparse_checkformat(a, SPARSE_CCS, true, true) &&
          sparse_checkformat(b, SPARSE_CCS, true, true)) ) return SPARSE_CONVFAILED;
    
    if (a->ccs.ncols!=b->ccs.ncols || a->ccs.nrows != b->ccs.nrows) return SPARSE_INCMPTBLDIM;
    sparsedok_clear(&out->dok);
    sparseccs_clear(&out->ccs);
#ifdef MORPHO_LINALG_USE_CSPARSE
    cs A, B;
    sparse_ccstocsparse(&a->ccs, &A);
    sparse_ccstocsparse(&b->ccs, &B);
    cs *ret=cs_add(&A, &B, alpha, beta);
    if (ret) {
        sparse_csparsetoccs(ret, &out->ccs);
        free(ret);
        return SPARSE_OK;
    }
#endif
    return SPARSE_FAILED;
}

/** Multiply two matrices
 * @param[in] a - sparse matrix
 * @param[in] b - sparse matrix
 * @param[out] out - a*b. */
objectsparseerror sparse_mul(objectsparse *a, objectsparse *b, objectsparse *out) {
    if (!(sparse_checkformat(a, SPARSE_CCS, true, true) &&
          sparse_checkformat(b, SPARSE_CCS, true, true)) ) return SPARSE_CONVFAILED;
    if (a->ccs.ncols!=b->ccs.nrows) return SPARSE_INCMPTBLDIM;
    sparsedok_clear(&out->dok);
    sparseccs_clear(&out->ccs);
    
#ifdef MORPHO_LINALG_USE_CSPARSE
    cs A, B;
    sparse_ccstocsparse(&a->ccs, &A);
    sparse_ccstocsparse(&b->ccs, &B);
    cs *ret=cs_multiply(&A, &B);
    if (ret) {
        sparse_csparsetoccs(ret, &out->ccs);
        free(ret);
        return SPARSE_OK;
    }
#endif
    return SPARSE_FAILED;
}

/** Multiply two matrices
 * @param[in] a - sparse matrix
 * @param[in] b - dense rhs (may have more than one column)
 * @param[out] out - Solution to a.x = b. */
objectsparseerror sparse_div(objectsparse *a, objectmatrix *b, objectmatrix *out) {
    if (!(sparse_checkformat(a, SPARSE_CCS, true, true))) return SPARSE_CONVFAILED;
    if (a->ccs.ncols!=b->nrows || b->nrows!=out->nrows || b->ncols!=out->ncols) return SPARSE_INCMPTBLDIM;
    
    if (b!=out) cblas_dcopy(b->ncols * b->nrows, b->elements, 1, out->elements, 1);
    
#ifdef MORPHO_LINALG_USE_CSPARSE
    cs A;
    sparse_ccstocsparse(&a->ccs, &A);
    int ret=false;
    if (a->ccs.ncols==a->ccs.nrows) {
        ret=cs_lusol(0, &A, out->elements, MORPHO_EPS);
    } else {
        ret=cs_qrsol(0, &A, out->elements);
    }
    
    if (ret) return SPARSE_OK;
#endif
    
    return SPARSE_FAILED;
}

/** Multiply two matrices
 * @param[in] a - sparse matrix
 * @param[out] out - transpose(A). */
objectsparseerror sparse_transpose(objectsparse *a, objectsparse *out) {
    if (!(sparse_checkformat(a, SPARSE_CCS, true, true)) ) return SPARSE_CONVFAILED;
    sparsedok_clear(&out->dok);
    sparseccs_clear(&out->ccs);
    
#ifdef MORPHO_LINALG_USE_CSPARSE
    cs A;
    sparse_ccstocsparse(&a->ccs, &A);
    cs *ret=cs_transpose(&A, true);
    if (ret) {
        sparse_csparsetoccs(ret, &out->ccs);
        free(ret);
        return SPARSE_OK;
    }
#endif
    return SPARSE_FAILED;
}

/** Clears any data attached to a sparse matrix */
void sparse_clear(objectsparse *a) {
    sparsedok_clear(&a->dok);
    sparseccs_clear(&a->ccs);
}

/** Calculate the size of a sparse matrix structure */
size_t sparse_size(objectsparse *a) {
    return sizeof(objectsparse)+
           a->dok.dict.capacity*sizeof(dictionaryentry) +
           sizeof(int)*(a->ccs.ncols+1) +
           sizeof(int)*(a->ccs.nentries) +
           ( a->ccs.values ? sizeof(double)*(a->ccs.nentries) : 0);
}

/* ***************************************
 * Sparse builtin class
 * *************************************** */

void sparse_raiseerror(vm *v, objectsparseerror err) {
    switch(err) {
        case SPARSE_OK: break;
        case SPARSE_INCMPTBLDIM: morpho_runtimeerror(v, MATRIX_INCOMPATIBLEMATRICES); break;
        case SPARSE_CONVFAILED: morpho_runtimeerror(v, SPARSE_CONVFAILEDERR); break;
        case SPARSE_FAILED: morpho_runtimeerror(v, SPARSE_OPFAILEDERR); break;
    }
}

/** Constructs a Matrix object */
value sparse_constructor(vm *v, int nargs, value *args) {
    int nrows, ncols;
    objectsparse *new=NULL;
    value out=MORPHO_NIL; 
    
    if ( nargs==2 &&
         MORPHO_ISINTEGER(MORPHO_GETARG(args, 0)) &&
         MORPHO_ISINTEGER(MORPHO_GETARG(args, 1)) ) {
        nrows = MORPHO_GETINTEGERVALUE(MORPHO_GETARG(args, 0));
        ncols = MORPHO_GETINTEGERVALUE(MORPHO_GETARG(args, 1));
        new=object_newsparse(&nrows, &ncols);
    } else if (nargs==1 &&
               MORPHO_ISINTEGER(MORPHO_GETARG(args, 0))) {
        nrows = MORPHO_GETINTEGERVALUE(MORPHO_GETARG(args, 0));
        ncols = 1;
        new=object_newsparse(&nrows, &ncols);
    } else if (nargs==1 &&
               MORPHO_ISARRAY(MORPHO_GETARG(args, 0))) {
        new=object_sparsefromarray(MORPHO_GETARRAY(MORPHO_GETARG(args, 0)));
       if (!new) morpho_runtimeerror(v, SPARSE_INVLDARRAYINIT);
    } else if (nargs==1 &&
               MORPHO_ISLIST(MORPHO_GETARG(args, 0))) {
        new=object_sparsefromlist(MORPHO_GETLIST(MORPHO_GETARG(args, 0)));
       if (!new) morpho_runtimeerror(v, SPARSE_INVLDARRAYINIT);
    } else {
        morpho_runtimeerror(v, SPARSE_CONSTRUCTOR);
    }
    
    if (new) {
        out=MORPHO_OBJECT(new);
        morpho_bindobjects(v, 1, &out);
    }
    
    return out;
}

/** Retrieve a matrix element */
value Sparse_getindex(vm *v, int nargs, value *args) {
    objectsparse *s=MORPHO_GETSPARSE(MORPHO_SELF(args));
    unsigned int indx[2]={0,0};
    value out = MORPHO_FLOAT(0.0);
    
    if (array_valuestoindices(nargs, args+1, indx)) {
        sparse_getelement(s, indx[0], indx[1], &out);
    } else morpho_runtimeerror(v, MATRIX_INVLDINDICES);
    
    return out;
}

/** Set a matrix element */
value Sparse_setindex(vm *v, int nargs, value *args) {
    objectsparse *s=MORPHO_GETSPARSE(MORPHO_SELF(args));
    unsigned int indx[2]={0,0};
    
    if (array_valuestoindices(nargs-1, args+1, indx)) {
        double val=0.0;
        if (MORPHO_ISFLOAT(args[nargs])) val=MORPHO_GETFLOATVALUE(args[nargs]);
        if (MORPHO_ISINTEGER(args[nargs])) val=(double) MORPHO_GETINTEGERVALUE(args[nargs]);

        if (!sparse_setelement(s, indx[0], indx[1], MORPHO_FLOAT(val))) {
            morpho_runtimeerror(v, SPARSE_SETFAILED);
        }
    } else morpho_runtimeerror(v, MATRIX_INVLDINDICES);
    
    return MORPHO_NIL;
}

/** Enumerate protocol */
value Sparse_enumerate(vm *v, int nargs, value *args) {
    objectsparse *s=MORPHO_GETSPARSE(MORPHO_SELF(args));
    value out=MORPHO_NIL;
    
    if (nargs==1) {
        if (MORPHO_ISINTEGER(MORPHO_GETARG(args, 0))) {
            int i=MORPHO_GETINTEGERVALUE(MORPHO_GETARG(args, 0));
            
            sparse_enumerate(s, i, &out);
        }
    }
    
    return out;
}

/** Print a sparse matrix */
value Sparse_print(vm *v, int nargs, value *args) {
    objectsparse *s=MORPHO_GETSPARSE(MORPHO_SELF(args));
    
    if (sparse_checkformat(s, SPARSE_CCS, false, false)) {
        sparseccs_print(&s->ccs);
    } else if (sparse_checkformat(s, SPARSE_DOK, false, false)) {
        sparsedok_print(&s->dok);
    }
    
    return MORPHO_NIL;
}

/** Add two sparse matrices */
value Sparse_add(vm *v, int nargs, value *args) {
    objectsparse *a=MORPHO_GETSPARSE(MORPHO_SELF(args));
    value out=MORPHO_NIL;
 
    if (nargs==1 && MORPHO_ISSPARSE(MORPHO_GETARG(args, 0))) {
        objectsparse *b=MORPHO_GETSPARSE(MORPHO_GETARG(args, 0));
        
        objectsparse *new = object_newsparse(NULL, NULL);
        if (new) {
            objectsparseerror err =sparse_add(a, b, 1.0, 1.0, new);
            if (err==SPARSE_OK) {
                out=MORPHO_OBJECT(new);
                morpho_bindobjects(v, 1, &out);
            } else {
                sparse_raiseerror(v, err);
            }
        } else morpho_runtimeerror(v, ERROR_ALLOCATIONFAILED);
    }
    
    return out;
}

/** Subtract sparse matrices */
value Sparse_sub(vm *v, int nargs, value *args) {
    objectsparse *a=MORPHO_GETSPARSE(MORPHO_SELF(args));
    value out=MORPHO_NIL;
 
    if (nargs==1 && MORPHO_ISSPARSE(MORPHO_GETARG(args, 0))) {
        objectsparse *b=MORPHO_GETSPARSE(MORPHO_GETARG(args, 0));
        
        objectsparse *new = object_newsparse(NULL, NULL);
        if (new) {
            objectsparseerror err =sparse_add(a, b, 1.0, -1.0, new);
            if (err==SPARSE_OK) {
                out=MORPHO_OBJECT(new);
                morpho_bindobjects(v, 1, &out);
            } else {
                sparse_raiseerror(v, err);
            }
        } else morpho_runtimeerror(v, ERROR_ALLOCATIONFAILED);
    }
    
    return out;
}

/** Multiply sparse matrices */
value Sparse_mul(vm *v, int nargs, value *args) {
    objectsparse *a=MORPHO_GETSPARSE(MORPHO_SELF(args));
    value out=MORPHO_NIL;
 
    if (nargs==1 && MORPHO_ISSPARSE(MORPHO_GETARG(args, 0))) {
        objectsparse *b=MORPHO_GETSPARSE(MORPHO_GETARG(args, 0));
        
        objectsparse *new = object_newsparse(NULL, NULL);
        if (new) {
            objectsparseerror err =sparse_mul(a, b, new);
            if (err==SPARSE_OK) {
                out=MORPHO_OBJECT(new);
                morpho_bindobjects(v, 1, &out);
            } else {
                sparse_raiseerror(v, err);
            }
        } else morpho_runtimeerror(v, ERROR_ALLOCATIONFAILED);
    }
    
    return out;
}

/** Sparse rhs not implemented */
value Sparse_div(vm *v, int nargs, value *args) {
    return MORPHO_NIL;
}

/** Solve a linear system b/ A where A is sparse */
value Sparse_divr(vm *v, int nargs, value *args) {
    objectsparse *a=MORPHO_GETSPARSE(MORPHO_SELF(args));
    value out=MORPHO_NIL;
 
    if (nargs==1 && MORPHO_ISMATRIX(MORPHO_GETARG(args, 0))) {
        objectmatrix *b=MORPHO_GETMATRIX(MORPHO_GETARG(args, 0));
        
        objectmatrix *new = object_newmatrix(b->nrows, b->ncols, false);
        if (new) {
            objectsparseerror err =sparse_div(a, b, new);
            if (err==SPARSE_OK) {
                out=MORPHO_OBJECT(new);
                morpho_bindobjects(v, 1, &out);
            } else {
                sparse_raiseerror(v, err);
            }
        } else morpho_runtimeerror(v, ERROR_ALLOCATIONFAILED);
    }
    
    return out;
}

/** Multiply sparse matrices */
value Sparse_transpose(vm *v, int nargs, value *args) {
    objectsparse *a=MORPHO_GETSPARSE(MORPHO_SELF(args));
    value out=MORPHO_NIL;
 
    objectsparse *new = object_newsparse(NULL, NULL);
    if (new) {
        objectsparseerror err =sparse_transpose(a, new);
        if (err==SPARSE_OK) {
            out=MORPHO_OBJECT(new);
            morpho_bindobjects(v, 1, &out);
        } else {
            sparse_raiseerror(v, err);
        }
    } else morpho_runtimeerror(v, ERROR_ALLOCATIONFAILED);
    
    return out;
}

/** Clone a sparse matrix */
value Sparse_clone(vm *v, int nargs, value *args) {
    objectsparse *s=MORPHO_GETSPARSE(MORPHO_SELF(args));
    value out = MORPHO_NIL;
    objectsparse *new=sparse_clone(s);
    
    if (new) {
        out=MORPHO_OBJECT(new);
        morpho_bindobjects(v, 1, &out);
    }
    
    return out;
}

/** Count number of elements */
value Sparse_count(vm *v, int nargs, value *args) {
    objectsparse *s=MORPHO_GETSPARSE(MORPHO_SELF(args));
    value out = MORPHO_INTEGER(0);
    
    if (sparse_checkformat(s, SPARSE_DOK, false, false)) {
        out=MORPHO_INTEGER(sparsedok_count(&s->dok));
    } else if (sparse_checkformat(s, SPARSE_CCS, false, false)) {
        out=MORPHO_INTEGER(sparseccs_count(&s->ccs));
    }
    
    return out;
}

/** Sparse dimensions */
value Sparse_dimensions(vm *v, int nargs, value *args) {
    objectsparse *s=MORPHO_GETSPARSE(MORPHO_SELF(args));
    value dim[2];
    value out=MORPHO_NIL;
    
    if (s->ccs.ncols>0) {
        dim[0]=MORPHO_INTEGER(s->ccs.nrows);
        dim[1]=MORPHO_INTEGER(s->ccs.ncols);
    } else {
        dim[0]=MORPHO_INTEGER(s->dok.nrows);
        dim[1]=MORPHO_INTEGER(s->dok.ncols);
    }
    
    objectlist *new=object_newlist(2, dim);
    if (new) {
        out=MORPHO_OBJECT(new);
        morpho_bindobjects(v, 1, &out);
    } else morpho_runtimeerror(v, ERROR_ALLOCATIONFAILED);
    
    return out;
}

/** Get the row indices given a column */
value Sparse_rowindices(vm *v, int nargs, value *args) {
    objectsparse *s=MORPHO_GETSPARSE(MORPHO_SELF(args));
    value out=MORPHO_NIL;
 
    if (nargs==1 && MORPHO_ISINTEGER(MORPHO_GETARG(args, 0))) {
        if (sparse_checkformat(s, SPARSE_CCS, true, true)) {
            int col = MORPHO_GETINTEGERVALUE(MORPHO_GETARG(args, 0));
            int nentries=0, *entries=NULL;
            
            if (col<s->ccs.ncols) {
                if (sparseccs_getrowindices(&s->ccs, col, &nentries, &entries)) {
                    objectlist *new = object_newlist(nentries, NULL);
                    if (new) {
                        for (int i=0; i<nentries; i++) list_append(new, MORPHO_INTEGER(entries[i]));
                        
                        out=MORPHO_OBJECT(new);
                        morpho_bindobjects(v, 1, &out);
                    } else morpho_runtimeerror(v, ERROR_ALLOCATIONFAILED);
                }
            } else morpho_runtimeerror(v, MATRIX_INDICESOUTSIDEBOUNDS);
        }
    }
    
    return out;
}

/** Get the row indices given a column */
value Sparse_setrowindices(vm *v, int nargs, value *args) {
    objectsparse *s=MORPHO_GETSPARSE(MORPHO_SELF(args));
    value out=MORPHO_NIL;
 
    if (nargs==2 && MORPHO_ISINTEGER(MORPHO_GETARG(args, 0)) &&
        MORPHO_ISLIST(MORPHO_GETARG(args, 1))) {
        if (sparse_checkformat(s, SPARSE_CCS, true, true)) {
            int col = MORPHO_GETINTEGERVALUE(MORPHO_GETARG(args, 0));
            objectlist *list = MORPHO_GETLIST(MORPHO_GETARG(args, 1));
            int nentries=list_length(list);
            int entries[nentries];
            
            if (col<s->ccs.ncols) {
                for (int i=0; i<nentries; i++) {
                    value entry;
                    if (list_getelement(list, i, &entry) &&
                        MORPHO_ISINTEGER(entry)) {
                        entries[i]=MORPHO_GETINTEGERVALUE(entry);
                    } else { morpho_runtimeerror(v, MATRIX_INVLDINDICES); return MORPHO_NIL; }
                }
                
                if (!sparseccs_setrowindices(&s->ccs, col, nentries, entries)) {
                    morpho_runtimeerror(v, MATRIX_INCOMPATIBLEMATRICES);
                }
                
            } else morpho_runtimeerror(v, MATRIX_INDICESOUTSIDEBOUNDS);
        }
    }
    
    return out;
}

/** Get the column indices */
value Sparse_colindices(vm *v, int nargs, value *args) {
    objectsparse *s=MORPHO_GETSPARSE(MORPHO_SELF(args));
    value out=MORPHO_NIL;
 
    if (sparse_checkformat(s, SPARSE_CCS, true, true)) {
        int ncols=0;
        varray_int cols;
        varray_intinit(&cols);
        varray_intresize(&cols, s->ccs.ncols);
        if (sparseccs_getcolindices(&s->ccs, s->ccs.ncols, &ncols, cols.data)) {
            objectlist *new=object_newlist(ncols, NULL);
            if (new) {
                for (int i=0; i<ncols; i++) new->val.data[i]=MORPHO_INTEGER(cols.data[i]);
                new->val.count=ncols;
                out=MORPHO_OBJECT(new);
                morpho_bindobjects(v, 1, &out);
            }
        }
        
        varray_intclear(&cols);
    }
    
    return out;
}

/** Get a list of indices */
value Sparse_indices(vm *v, int nargs, value *args) {
    objectsparse *s=MORPHO_GETSPARSE(MORPHO_SELF(args));
    value out=MORPHO_NIL;
 
    if (sparse_checkformat(s, SPARSE_DOK, true, true)) {
        objectlist *list=object_newlist(s->dok.dict.count, NULL);
        if (list) {
            for (objectdokkey *key=s->dok.keys; key!=NULL; key=(objectdokkey *) key->obj.next) {
                objectlist *entry=object_newlist(2, NULL);
                if (entry) {
                    list_append(entry, MORPHO_INTEGER(key->row));
                    list_append(entry, MORPHO_INTEGER(key->col));
                    list_append(list, MORPHO_OBJECT(entry));
                } else morpho_runtimeerror(v, ERROR_ALLOCATIONFAILED);
            }
            /* Temporarily append a self reference so everything is in one place to bind... */
            list_append(list, MORPHO_OBJECT(list));
            morpho_bindobjects(v, list->val.count, list->val.data);
            list->val.count--; // And pop it back off
            out = MORPHO_OBJECT(list);
        } else morpho_runtimeerror(v, ERROR_ALLOCATIONFAILED);
    }
    
    return out;
}

MORPHO_BEGINCLASS(Sparse)
MORPHO_METHOD(MORPHO_GETINDEX_METHOD, Sparse_getindex, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(MORPHO_SETINDEX_METHOD, Sparse_setindex, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(MORPHO_ENUMERATE_METHOD, Sparse_enumerate, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(MORPHO_PRINT_METHOD, Sparse_print, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(MORPHO_ADD_METHOD, Sparse_add, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(MORPHO_SUB_METHOD, Sparse_sub, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(MORPHO_MUL_METHOD, Sparse_mul, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(MORPHO_DIVR_METHOD, Sparse_divr, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(MATRIX_TRANSPOSE_METHOD, Sparse_transpose, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(MORPHO_COUNT_METHOD, Sparse_count, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(MATRIX_DIMENSIONS_METHOD, Sparse_dimensions, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(SPARSE_ROWINDICES_METHOD, Sparse_rowindices, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(SPARSE_SETROWINDICES_METHOD, Sparse_setrowindices, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(SPARSE_COLINDICES_METHOD, Sparse_colindices, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(MORPHO_CLONE_METHOD, Sparse_clone, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(SPARSE_INDICES_METHOD, Sparse_indices, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ***************************************
 * Initialization
 * *************************************** */

void sparse_initialize(void) {
    builtin_addfunction(SPARSE_CLASSNAME, sparse_constructor, BUILTIN_FLAGSEMPTY);
    
    value sparseclass=builtin_addclass(SPARSE_CLASSNAME, MORPHO_GETCLASSDEFINITION(Sparse), MORPHO_NIL);
    builtin_setveneerclass(OBJECT_SPARSE, sparseclass);
    
    morpho_defineerror(SPARSE_CONSTRUCTOR, ERROR_HALT, SPARSE_CONSTRUCTOR_MSG);
    morpho_defineerror(SPARSE_SETFAILED, ERROR_HALT, SPARSE_SETFAILED_MSG);
    morpho_defineerror(SPARSE_INVLDARRAYINIT, ERROR_HALT, SPARSE_INVLDARRAYINIT_MSG);
    morpho_defineerror(SPARSE_CONVFAILEDERR, ERROR_HALT, SPARSE_CONVFAILEDERR_MSG);    
    morpho_defineerror(SPARSE_OPFAILEDERR, ERROR_HALT, SPARSE_OPFAILEDERR_MSG);
    
    //sparse_test();
}
