/** @file functional.c
 *  @author T J Atherton
 *
 *  @brief Functionals
 */

#include "functional.h"
#include "builtin.h"
#include "veneer.h"
#include "common.h"
#include "error.h"
#include "value.h"
#include "object.h"
#include "morpho.h"
#include "matrix.h"
#include "sparse.h"
#include "mesh.h"
#include "field.h"
#include "selection.h"
#include "integrate.h"
#include <math.h>

#ifndef M_PI
    #define M_PI 3.14159265358979323846
#endif

static value functional_gradeproperty;
static value functional_fieldproperty;
//static value functional_functionproperty;

/** Symmetry behaviors */
typedef enum {
    SYMMETRY_NONE,
    SYMMETRY_ADD
} symmetrybhvr;

/** Integrand function */
typedef bool (functional_integrand) (vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out);

/** Gradient function */
typedef bool (functional_gradient) (vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, objectmatrix *frc);

struct s_functional_mapinfo; // Resolve circular typedef dependency

/** Dependencies function */
typedef bool (functional_dependencies) (struct s_functional_mapinfo *info, elementid id, varray_elementid *out);

typedef struct s_functional_mapinfo {
    objectmesh *mesh; // Mesh to use
    objectselection *sel; // Selection, if any
    objectfield *field; // Field, if any
    grade g; // Grade to use
    functional_integrand *integrand; // Integrand function
    functional_gradient *grad; // Gradient
    functional_dependencies *dependencies; // Dependencies
    symmetrybhvr sym; // Symmetry behavior
    void *ref; // Reference to pass on
} functional_mapinfo;



/* **********************************************************************
 * Utility functions
 * ********************************************************************** */

static void functional_clearmapinfo(functional_mapinfo *info) {
    info->mesh=NULL;
    info->field=NULL;
    info->sel=NULL;
    info->g=0;
    info->integrand=NULL;
    info->grad=NULL;
    info->dependencies=NULL;
    info->ref=NULL;
    info->sym=SYMMETRY_NONE;
}

/** Validates the arguments provided to a functional
 * @param[in] v - vm
 * @param[in] nargs - number of arguments
 * @param[in] args - the arguments
 * @param[out] info - mapinfo block  */
bool functional_validateargs(vm *v, int nargs, value *args, functional_mapinfo *info) {
    functional_clearmapinfo(info);
    
    for (unsigned int i=0; i<nargs; i++) {
        if (MORPHO_ISMESH(MORPHO_GETARG(args,i))) {
            info->mesh = MORPHO_GETMESH(MORPHO_GETARG(args,i));
        } else if (MORPHO_ISSELECTION(MORPHO_GETARG(args,i))) {
            info->sel = MORPHO_GETSELECTION(MORPHO_GETARG(args,i));
        } else if (MORPHO_ISFIELD(MORPHO_GETARG(args,i))) {
            info->field = MORPHO_GETFIELD(MORPHO_GETARG(args,i));
            if (info->field) info->mesh = (info->field->mesh); // Retrieve the mesh from the field
        }
    }

    
    if (info->mesh) return true;
    morpho_runtimeerror(v, FUNC_INTEGRAND_MESH);
    return false;
}


/* **********************************************************************
 * Common routines
 * ********************************************************************** */

/** Internal function to count the number of elements */
static bool functional_countelements(vm *v, objectmesh *mesh, grade g, int *n, objectsparse **s) {
    /* How many elements? */
    if (g==MESH_GRADE_VERTEX) {
        *n=mesh->vert->ncols;
    } else {
        *s=mesh_getconnectivityelement(mesh, 0, g);
        if (*s) {
            *n=(*s)->ccs.ncols; // Number of elements
        } else {
            morpho_runtimeerror(v, FUNC_ELNTFND, g);
            return false;
        }
    }
    return true;
}

static int functional_symmetryimagelistfn(const void *a, const void *b) {
    elementid i=*(elementid *) a; elementid j=*(elementid *) b;
    return (int) i-j;
}

/** Gets a list of all image elements (those that map onto a target element)
 * @param[in] mesh - the mesh
 * @param[in] g - grade to look up
 * @param[in] sort - whether to sort othe results
 * @param[out] ids - varray is filled with image element ids */
void functional_symmetryimagelist(objectmesh *mesh, grade g, bool sort, varray_elementid *ids) {
    objectsparse *conn=mesh_getconnectivityelement(mesh, g, g);
    
    ids->count=0; // Initialize the varray
    
    if (conn) {
        int i,j;
        void *ctr=sparsedok_loopstart(&conn->dok);
        
        while (sparsedok_loop(&conn->dok, &ctr, &i, &j)) {
            varray_elementidwrite(ids, j);
        }
        
        if (sort) qsort(ids->data, ids->count, sizeof(elementid), functional_symmetryimagelistfn);
    }
}

/** Sums forces on symmetry vertices
 * @param[in] mesh - mesh object
 * @param frc - force object; updated if symmetries are present. */
bool functional_symmetrysumforces(objectmesh *mesh, objectmatrix *frc) {
    objectsparse *s=mesh_getconnectivityelement(mesh, 0, 0); // Checking for vertex symmetries
    
    if (s) {
        int i,j;
        void *ctr = sparsedok_loopstart(&s->dok);
        double *fi, *fj, fsum[mesh->dim];
        
        while (sparsedok_loop(&s->dok, &ctr, &i, &j)) {
            if (matrix_getcolumn(frc, i, &fi) &&
                matrix_getcolumn(frc, j, &fj)) {
                
                for (unsigned int k=0; k<mesh->dim; k++) fsum[k]=fi[k]+fj[k];
                matrix_setcolumn(frc, i, fsum);
                matrix_setcolumn(frc, j, fsum);
            }
        }
    }
    
    return s;
}

bool functional_inlist(varray_elementid *list, elementid id) {
    for (unsigned int i=0; i<list->count; i++) if (list->data[i]==id) return true;
    return false;
}

bool functional_containsvertex(int nv, int *vid, elementid id) {
    for (unsigned int i=0; i<nv; i++) if (vid[i]==id) return true;
    return false;
}

/* **********************************************************************
 * Map functions
 * ********************************************************************** */

/** Sums an integrand
 * @param[in] v - virtual machine in use
 * @param[in] info - map info
 * @param[out] out - a matrix of integrand values
 * @returns true on success, false otherwise. Error reporting through VM. */
bool functional_sumintegrand(vm *v, functional_mapinfo *info, value *out) {
    bool success=false;
    objectmesh *mesh = info->mesh;
    objectselection *sel = info->sel;
    grade g = info->g;
    functional_integrand *integrand = info->integrand;
    void *ref = info->ref;
    objectsparse *s=NULL;
    int n=0;
    
    if (!functional_countelements(v, mesh, g, &n, &s)) return false;
    
    /* Find any image elements so we can skip over them */
    varray_elementid imageids;
    varray_elementidinit(&imageids);
    functional_symmetryimagelist(mesh, g, true, &imageids);
    
    if (n>0) {
        int vertexid; // Use this if looping over grade 0
        int *vid=(g==0 ? &vertexid : NULL),
            nv=(g==0 ? 1 : 0); // The vertex indices
        int sindx=0; // Index into imageids array
        double sum=0.0, c=0.0, y, t, result;
        
        if (sel) { // Loop over selection
            if (sel->selected[g].count>0) for (unsigned int k=0; k<sel->selected[g].capacity; k++) {
                if (!MORPHO_ISINTEGER(sel->selected[g].contents[k].key)) continue;
                elementid i = MORPHO_GETINTEGERVALUE(sel->selected[g].contents[k].key);
                
                // Skip this element if it's an image element:
                if ((imageids.count>0) && (sindx<imageids.count) && imageids.data[sindx]==i) { sindx++; continue; }
                
                if (s) sparseccs_getrowindices(&s->ccs, i, &nv, &vid);
                else vertexid=i;
            
                if (vid && nv>0) {
                    if ((*integrand) (v, mesh, i, nv, vid, ref, &result)) {
                        y=result-c; t=sum+y; c=(t-sum)-y; sum=t; // Kahan summation
                    } else goto functional_sumintegrand_cleanup;
                }
            }
        } else { // Loop over elements
            for (elementid i=0; i<n; i++) {
                // Skip this element if it's an image element
                if ((imageids.count>0) && (sindx<imageids.count) && imageids.data[sindx]==i) { sindx++; continue; }
                
                if (s) sparseccs_getrowindices(&s->ccs, i, &nv, &vid);
                else vertexid=i;

                if (vid && nv>0) {
                    if ((*integrand) (v, mesh, i, nv, vid, ref, &result)) {
                        y=result-c; t=sum+y; c=(t-sum)-y; sum=t; // Kahan summation
                    } else goto functional_sumintegrand_cleanup;
                }
            }
        }
        
        *out=MORPHO_FLOAT(sum);
    }
    
    success=true;
    
functional_sumintegrand_cleanup:
    varray_elementidclear(&imageids);
    return success;
}

/** Calculate an integrand
 * @param[in] v - virtual machine in use
 * @param[in] info - map info
 * @param[out] out - a matrix of integrand values
 * @returns true on success, false otherwise. Error reporting through VM. */
bool functional_mapintegrand(vm *v, functional_mapinfo *info, value *out) {
    objectmesh *mesh = info->mesh;
    objectselection *sel = info->sel;
    grade g = info->g;
    functional_integrand *integrand = info->integrand;
    void *ref = info->ref;
    objectsparse *s=NULL;
    objectmatrix *new=NULL;
    bool ret=false;
    int n=0;
        
    /* How many elements? */
    if (!functional_countelements(v, mesh, g, &n, &s)) return false;
    
    /* Find any image elements so we can skip over them */
    varray_elementid imageids;
    varray_elementidinit(&imageids);
    functional_symmetryimagelist(mesh, g, true, &imageids);
    
    /* Create the output matrix */
    if (n>0) {
        new=object_newmatrix(1, n, true);
        if (!new) { morpho_runtimeerror(v, ERROR_ALLOCATIONFAILED); return false; }
    }
    
    if (new) {
        int vertexid; // Use this if looping over grade 0
        int *vid=(g==0 ? &vertexid : NULL),
            nv=(g==0 ? 1 : 0); // The vertex indices
        int sindx=0; // Index into imageids array
        double result;
        
        if (sel) { // Loop over selection
            if (sel->selected[g].count>0) for (unsigned int k=0; k<sel->selected[g].capacity; k++) {
                if (!MORPHO_ISINTEGER(sel->selected[g].contents[k].key)) continue;
                elementid i = MORPHO_GETINTEGERVALUE(sel->selected[g].contents[k].key);
                
                // Skip this element if it's an image element
                if ((imageids.count>0) && (sindx<imageids.count) && imageids.data[sindx]==i) { sindx++; continue; }
                
                if (s) sparseccs_getrowindices(&s->ccs, i, &nv, &vid);
                else vertexid=i;
            
                if (vid && nv>0) {
                    if ((*integrand) (v, mesh, i, nv, vid, ref, &result)) {
                        matrix_setelement(new, 0, i, result);
                    } else goto functional_mapintegrand_cleanup;
                }
            }
        } else { // Loop over elements
            for (elementid i=0; i<n; i++) {
                // Skip this element if it's an image element
                if ((imageids.count>0) && (sindx<imageids.count) && imageids.data[sindx]==i) { sindx++; continue; }
                
                if (s) sparseccs_getrowindices(&s->ccs, i, &nv, &vid);
                else vertexid=i;

                if (vid && nv>0) {
                    if ((*integrand) (v, mesh, i, nv, vid, ref, &result)) {
                        matrix_setelement(new, 0, i, result);
                    } else goto functional_mapintegrand_cleanup;
                }
            }
        }
        *out = MORPHO_OBJECT(new);
        ret=true;
    }
    
    varray_elementidclear(&imageids);
    return ret;
    
functional_mapintegrand_cleanup:
    object_free((object *) new);
    varray_elementidclear(&imageids);
    return false;
}

/** Calculate gradient
 * @param[in] v - virtual machine in use
 * @param[in] info - map info structure
 * @param[out] out - a matrix of integrand values
 * @returns true on success, false otherwise. Error reporting through VM. */
bool functional_mapgradient(vm *v, functional_mapinfo *info, value *out) {
    objectmesh *mesh = info->mesh;
    objectselection *sel = info->sel;
    grade g = info->g;
    functional_gradient *grad = info->grad;
    void *ref = info->ref;
    symmetrybhvr sym = info->sym;
    objectsparse *s=NULL;
    objectmatrix *frc=NULL;
    bool ret=false;
    int n=0;
    
    /* How many elements? */
    if (!functional_countelements(v, mesh, g, &n, &s)) return false;
    
    /* Create the output matrix */
    if (n>0) {
        frc=object_newmatrix(mesh->vert->nrows, mesh->vert->ncols, true);
        if (!frc)  { morpho_runtimeerror(v, ERROR_ALLOCATIONFAILED); return false; }
    }
    
    if (frc) {
        int vertexid; // Use this if looping over grade 0
        int *vid=(g==0 ? &vertexid : NULL),
            nv=(g==0 ? 1 : 0); // The vertex indices
            
        
        if (sel) { // Loop over selection
            if (sel->selected[g].count>0) for (unsigned int k=0; k<sel->selected[g].capacity; k++) {
                if (!MORPHO_ISINTEGER(sel->selected[g].contents[k].key)) continue;
                
                elementid i = MORPHO_GETINTEGERVALUE(sel->selected[g].contents[k].key);
                if (s) sparseccs_getrowindices(&s->ccs, i, &nv, &vid);
                else vertexid=i;
            
                if (vid && nv>0) {
                    if (!(*grad) (v, mesh, i, nv, vid, ref, frc)) goto functional_mapgradient_cleanup;
                }
            }
        } else { // Loop over elements
            for (elementid i=0; i<n; i++) {
                if (s) sparseccs_getrowindices(&s->ccs, i, &nv, &vid);
                else vertexid=i;

                if (vid && nv>0) {
                    if (!(*grad) (v, mesh, i, nv, vid, ref, frc)) goto functional_mapgradient_cleanup;
                }
            }
        }
        
        if (sym==SYMMETRY_ADD) functional_symmetrysumforces(mesh, frc);
        
        *out = MORPHO_OBJECT(frc);
        ret=true;
    }
    
functional_mapgradient_cleanup:
    if (!ret) object_free((object *) frc);
    
    return ret;
}

/* Calculates a numerical gradient */
static bool functional_numericalgradient(vm *v, objectmesh *mesh, elementid i, int nv, int *vid, functional_integrand *integrand, void *ref, objectmatrix *frc) {
    double f0,fp,fm,x0,eps=1e-10; // Should use sqrt(machineeps)*(1+|x|) here
    
    // Loop over vertices in element
    for (unsigned int j=0; j<nv; j++) {
        // Loop over coordinates
        for (unsigned int k=0; k<mesh->dim; k++) {
            matrix_getelement(frc, k, vid[j], &f0);
            
            matrix_getelement(mesh->vert, k, vid[j], &x0);
            matrix_setelement(mesh->vert, k, vid[j], x0+eps);
            if (!(*integrand) (v, mesh, i, nv, vid, ref, &fp)) return false;
            matrix_setelement(mesh->vert, k, vid[j], x0-eps);
            if (!(*integrand) (v, mesh, i, nv, vid, ref, &fm)) return false;
            matrix_setelement(mesh->vert, k, vid[j], x0);
            
            matrix_setelement(frc, k, vid[j], f0+(fp-fm)/(2*eps));
        }
    }
    return true;
}

/* Calculates a numerical gradient for a remote vertex */
static bool functional_numericalremotegradientold(vm *v, functional_mapinfo *info, objectsparse *conn, elementid remoteid, elementid i, int nv, int *vid, objectmatrix *frc) {
    objectmesh *mesh = info->mesh;
    double f0,fp,fm,x0,eps=1e-10; // Should use sqrt(machineeps)*(1+|x|) here
    
    int *rvid=(info->g==0 ? &remoteid : NULL),
        rnv=(info->g==0 ? 1 : 0); // The vertex indices
    
    if (conn) sparseccs_getrowindices(&conn->ccs, remoteid, &rnv, &rvid);
    
    // Loop over vertices in element
    for (unsigned int j=0; j<nv; j++) {
        // Loop over coordinates
        for (unsigned int k=0; k<mesh->dim; k++) {
            matrix_getelement(frc, k, vid[j], &f0);
            
            matrix_getelement(mesh->vert, k, vid[j], &x0);
            matrix_setelement(mesh->vert, k, vid[j], x0+eps);
            if (!(*info->integrand) (v, mesh, remoteid, rnv, rvid, info->ref, &fp)) return false;
            matrix_setelement(mesh->vert, k, vid[j], x0-eps);
            if (!(*info->integrand) (v, mesh, remoteid, rnv, rvid, info->ref, &fm)) return false;
            matrix_setelement(mesh->vert, k, vid[j], x0);
            
            matrix_setelement(frc, k, vid[j], f0+(fp-fm)/(2*eps));
        }
    }
    
    return true;
}

static bool functional_numericalremotegradient(vm *v, functional_mapinfo *info, objectsparse *conn, elementid remoteid, elementid i, int nv, int *vid, objectmatrix *frc) {
    objectmesh *mesh = info->mesh;
    double f0,fp,fm,x0,eps=1e-10; // Should use sqrt(machineeps)*(1+|x|) here
    
    // Loop over coordinates
    for (unsigned int k=0; k<mesh->dim; k++) {
        matrix_getelement(frc, k, remoteid, &f0);
        
        matrix_getelement(mesh->vert, k, remoteid, &x0);
        matrix_setelement(mesh->vert, k, remoteid, x0+eps);
        if (!(*info->integrand) (v, mesh, i, nv, vid, info->ref, &fp)) return false;
        matrix_setelement(mesh->vert, k, remoteid, x0-eps);
        if (!(*info->integrand) (v, mesh, i, nv, vid, info->ref, &fm)) return false;
        matrix_setelement(mesh->vert, k, remoteid, x0);
        
        matrix_setelement(frc, k, remoteid, f0+(fp-fm)/(2*eps));
    }
    
    return true;
}

/** Map numerical gradient over the elements
 * @param[in] v - virtual machine in use
 * @param[in] info - map info
 * @param[out] out - a matrix of integrand values
 * @returns true on success, false otherwise. Error reporting through VM. */
bool functional_mapnumericalgradient(vm *v, functional_mapinfo *info, value *out) {
    objectmesh *mesh = info->mesh;
    objectselection *sel = info->sel;
    grade g = info->g;
    functional_integrand *integrand = info->integrand;
    void *ref = info->ref;
    symmetrybhvr sym = info->sym;
    objectsparse *s=NULL;
    objectmatrix *frc=NULL;
    bool ret=false;
    int n=0;
    
    varray_elementid dependencies;
    if (info->dependencies) varray_elementidinit(&dependencies);
    
    /* Find any image elements so we can skip over them */
    varray_elementid imageids;
    varray_elementidinit(&imageids);
    functional_symmetryimagelist(mesh, g, true, &imageids);
    
    /* How many elements? */
    if (!functional_countelements(v, mesh, g, &n, &s)) return false;
    
    /* Create the output matrix */
    if (n>0) {
        frc=object_newmatrix(mesh->vert->nrows, mesh->vert->ncols, true);
        if (!frc)  { morpho_runtimeerror(v, ERROR_ALLOCATIONFAILED); return false; }
    }
    
    if (frc) {
        int vertexid; // Use this if looping over grade 0
        int *vid=(g==0 ? &vertexid : NULL),
            nv=(g==0 ? 1 : 0); // The vertex indices
        int sindx=0; // Index into imageids array
        
        if (sel) { // Loop over selection
            if (sel->selected[g].count>0) for (unsigned int k=0; k<sel->selected[g].capacity; k++) {
                if (!MORPHO_ISINTEGER(sel->selected[g].contents[k].key)) continue;
                
                elementid i = MORPHO_GETINTEGERVALUE(sel->selected[g].contents[k].key);
                if (s) sparseccs_getrowindices(&s->ccs, i, &nv, &vid);
                else vertexid=i;
                
                // Skip this element if it's an image element
                if ((imageids.count>0) && (sindx<imageids.count) && imageids.data[sindx]==i) { sindx++; continue; }
            
                if (vid && nv>0) {
                    if (!functional_numericalgradient(v, mesh, i, nv, vid, integrand, ref, frc)) goto functional_numericalgradient_cleanup;
                    
                    if (info->dependencies && // Loop over dependencies if there are any
                        (info->dependencies) (info, i, &dependencies)) {
                        for (int j=0; j<dependencies.count; j++) {
                            if (!functional_numericalremotegradient(v, info, s, dependencies.data[j], i, nv, vid, frc)) goto functional_numericalgradient_cleanup;
                        }
                        dependencies.count=0;
                    }
                }
            }
        } else { // Loop over elements
            for (elementid i=0; i<n; i++) {
                // Skip this element if it's an image element
                if ((imageids.count>0) && (sindx<imageids.count) && imageids.data[sindx]==i) { sindx++; continue; }
                
                if (s) sparseccs_getrowindices(&s->ccs, i, &nv, &vid);
                else vertexid=i;

                if (vid && nv>0) {
                    
                    if (!functional_numericalgradient(v, mesh, i, nv, vid, integrand, ref, frc)) goto functional_numericalgradient_cleanup;
                
                    if (info->dependencies && // Loop over dependencies if there are any
                        (info->dependencies) (info, i, &dependencies)) {
                        for (int j=0; j<dependencies.count; j++) {
                            if (functional_containsvertex(nv, vid, dependencies.data[j])) continue;
                            if (!functional_numericalremotegradient(v, info, s, dependencies.data[j], i, nv, vid, frc)) goto functional_numericalgradient_cleanup;
                        }
                        dependencies.count=0;
                    }
                }
            }
        }
        
        if (sym==SYMMETRY_ADD) functional_symmetrysumforces(mesh, frc);
        
        *out = MORPHO_OBJECT(frc);
        ret=true;
    }

functional_numericalgradient_cleanup:
    varray_elementidclear(&imageids);
    if (info->dependencies) varray_elementidclear(&dependencies);
    if (!ret) object_free((object *) frc);
    
    return ret;
}

bool functional_mapnumericalfieldgradient(vm *v, functional_mapinfo *info, value *out) {
    objectmesh *mesh = info->mesh;
    objectselection *sel = info->sel;
    objectfield *field = info->field;
    grade grd = info->g;
    functional_integrand *integrand = info->integrand;
    void *ref = info->ref;
    //symmetrybhvr sym = info->sym;
    
    double eps=1e-10;
    bool ret=false;
    objectsparse *conn=mesh_getconnectivityelement(mesh, 0, grd); // Connectivity for the element
    
    /* Create the output field */
    objectfield *grad=object_newfield(mesh, field->prototype, field->dof);
    if (!grad) return false;
    
    field_zero(grad);
    
    /* Loop over elements in the field */
    for (grade g=0; g<field->ngrades; g++) {
        if (field->dof[g]==0) continue;
        int nentries=1, *entries, nv, *vid;
        double fr,fl;
        objectsparse *rconn=mesh_addconnectivityelement(mesh, grd, g); // Find dependencies for the grade
        
        for (elementid id=0; id<mesh_nelementsforgrade(mesh, g); id++) {
            entries = &id; // if there's no connectivity matrix, we'll just use the id itself
            
            if ((!rconn) || mesh_getconnectivity(rconn, id, &nentries, &entries)) {
                for (int i=0; i<nentries; i++) {
                    if (conn) {
                        if (sel) if (!selection_isselected(sel, grd, entries[i])) continue;
                        // Check selections here
                        sparseccs_getrowindices(&conn->ccs, entries[i], &nv, &vid);
                    } else {
                        if (sel) if (!selection_isselected(sel, grd, id)) continue;
                        nv=1; vid=&id; 
                    }
                    
                    /* Loop over dofs in field entry */
                    for (int j=0; j<field->psize*field->dof[g]; j++) {
                        int k=field->offset[g]+id*field->psize*field->dof[g]+j;
                        double fld=field->data.elements[k];
                        field->data.elements[k]+=eps;
                        
                        if (!(*integrand) (v, mesh, id, nv, vid, ref, &fr)) goto functional_mapnumericalfieldgradient_cleanup;
                        
                        field->data.elements[k]=fld-eps;
                        
                        if (!(*integrand) (v, mesh, id, nv, vid, ref, &fl)) goto functional_mapnumericalfieldgradient_cleanup;
                        
                        field->data.elements[k]=fld;
                        
                        grad->data.elements[k]+=(fr-fl)/(2*eps);
                    }
                }
            }
        }
        
        *out = MORPHO_OBJECT(grad);
        ret=true;
    }
    
functional_mapnumericalfieldgradient_cleanup:
    if (!ret) object_free((object *) grad);
    
    return ret;
}

/* **********************************************************************
 * Common library functions
 * ********************************************************************** */

/** Calculate the difference of two vectors */
void functional_vecadd(unsigned int n, double *a, double *b, double *out) {
    for (unsigned int i=0; i<n; i++) out[i]=a[i]+b[i];
}

/** Add with scale */
void functional_vecaddscale(unsigned int n, double *a, double lambda, double *b, double *out) {
    for (unsigned int i=0; i<n; i++) out[i]=a[i]+lambda*b[i];
}

/** Calculate the difference of two vectors */
void functional_vecsub(unsigned int n, double *a, double *b, double *out) {
    for (unsigned int i=0; i<n; i++) out[i]=a[i]-b[i];
}

/** Scale a vector */
void functional_vecscale(unsigned int n, double lambda, double *a, double *out) {
    for (unsigned int i=0; i<n; i++) out[i]=lambda*a[i];
}

/** Calculate the norm of a vector */
double functional_vecnorm(unsigned int n, double *a) {
    return cblas_dnrm2(n, a, 1);
}

/** Dot product of two vectors */
double functional_vecdot(unsigned int n, double *a, double *b) {
    return cblas_ddot(n, a, 1, b, 1);
}

/** 3D cross product  */
void functional_veccross(double *a, double *b, double *out) {
    out[0]=a[1]*b[2]-a[2]*b[1];
    out[1]=a[2]*b[0]-a[0]*b[2];
    out[2]=a[0]*b[1]-a[1]*b[0];
}

bool length_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out);
bool area_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out);
bool volume_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out);

/** Calculate element size */
bool functional_elementsize(vm *v, objectmesh *mesh, grade g, elementid id, int nv, int *vid, double *out) {
    switch (g) {
        case 1: return length_integrand(v, mesh, id, nv, vid, NULL, out);
        case 2: return area_integrand(v, mesh, id, nv, vid, NULL, out);
        case 3: return volume_integrand(v, mesh, id, nv, vid, NULL, out);
    }
    return false;
}

/* **********************************************************************
 * Functionals
 * ********************************************************************** */

/** Initialize a functional */
#define FUNCTIONAL_INIT(name, grade) value name##_init(vm *v, int nargs, value *args) { \
    objectinstance_setproperty(MORPHO_GETINSTANCE(MORPHO_SELF(args)), functional_gradeproperty, MORPHO_INTEGER(grade)); \
    return MORPHO_NIL; \
}

/** Evaluate an integrand */
#define FUNCTIONAL_INTEGRAND(name, grade, integrandfn) value name##_integrand(vm *v, int nargs, value *args) { \
    functional_mapinfo info; \
    value out=MORPHO_NIL; \
    \
    if (functional_validateargs(v, nargs, args, &info)) { \
        info.g = grade; info.integrand = integrandfn; \
        functional_mapintegrand(v, &info, &out); \
    } \
    if (!MORPHO_ISNIL(out)) morpho_bindobjects(v, 1, &out); \
    return out; \
}

/** Evaluate a gradient */
#define FUNCTIONAL_GRADIENT(name, grade, gradientfn, symbhvr) \
value name##_gradient(vm *v, int nargs, value *args) { \
    functional_mapinfo info; \
    value out=MORPHO_NIL; \
    \
    if (functional_validateargs(v, nargs, args, &info)) { \
        info.g = grade; info.grad = gradientfn; info.sym = symbhvr; \
        functional_mapgradient(v, &info, &out); \
    } \
    if (!MORPHO_ISNIL(out)) morpho_bindobjects(v, 1, &out); \
    \
    return out; \
}

/** Total an integrand */
#define FUNCTIONAL_TOTAL(name, grade, totalfn) \
value name##_total(vm *v, int nargs, value *args) { \
    functional_mapinfo info; \
    value out=MORPHO_NIL; \
    \
    if (functional_validateargs(v, nargs, args, &info)) { \
        info.g = grade; info.integrand = totalfn; \
        functional_sumintegrand(v, &info, &out); \
    } \
    \
    return out; \
}

/* Alternative way of defining methods that use a reference */
#define FUNCTIONAL_METHOD(class, name, grade, reftype, prepare, integrandfn, integrandmapfn, deps, err, symbhvr) value class##_##name(vm *v, int nargs, value *args) { \
    functional_mapinfo info; \
    reftype ref; \
    value out=MORPHO_NIL; \
    \
    if (functional_validateargs(v, nargs, args, &info)) { \
        if (prepare(MORPHO_GETINSTANCE(MORPHO_SELF(args)), info.mesh, grade, info.sel, &ref)) { \
            info.integrand = integrandmapfn; \
            info.dependencies = deps, \
            info.sym = symbhvr; \
            info.g = grade; \
            info.ref = &ref; \
            integrandfn(v, &info, &out); \
        } else morpho_runtimeerror(v, err); \
    } \
    if (!MORPHO_ISNIL(out)) morpho_bindobjects(v, 1, &out); \
    return out; \
}

/* ----------------------------------------------
 * Length
 * ---------------------------------------------- */

/** Calculate area */
bool length_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    double *x[nv], s0[mesh->dim];
    for (int j=0; j<nv; j++) matrix_getcolumn(mesh->vert, vid[j], &x[j]);
    
    functional_vecsub(mesh->dim, x[1], x[0], s0);
    
    *out=functional_vecnorm(mesh->dim, s0);
    return true;
}

/** Calculate gradient */
bool length_gradient(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, objectmatrix *frc) {
    double *x[nv], s0[mesh->dim], norm;
    for (int j=0; j<nv; j++) matrix_getcolumn(mesh->vert, vid[j], &x[j]);
    
    functional_vecsub(mesh->dim, x[1], x[0], s0);
    norm=functional_vecnorm(mesh->dim, s0);
    if (norm<MORPHO_EPS) return false;
    
    matrix_addtocolumn(frc, vid[0], -1.0/norm, s0);
    matrix_addtocolumn(frc, vid[1], 1./norm, s0);
    
    return true;
}

FUNCTIONAL_INIT(Length, MESH_GRADE_LINE)
FUNCTIONAL_INTEGRAND(Length, MESH_GRADE_LINE, length_integrand)
FUNCTIONAL_GRADIENT(Length, MESH_GRADE_LINE, length_gradient, SYMMETRY_ADD)
FUNCTIONAL_TOTAL(Length, MESH_GRADE_LINE, length_integrand)

MORPHO_BEGINCLASS(Length)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, Length_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, Length_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, Length_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, Length_total, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * Enclosed area
 * ---------------------------------------------- */

/** Calculate area enclosed */
bool areaenclosed_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    double *x[nv], cx[mesh->dim];
    for (int j=0; j<nv; j++) matrix_getcolumn(mesh->vert, vid[j], &x[j]);
    
    functional_veccross(x[0], x[1], cx);
    
    *out=0.5*functional_vecnorm(mesh->dim, cx);
    return true;
}

/** Calculate gradient */
bool areaenclosed_gradient(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, objectmatrix *frc) {
    double *x[nv], cx[mesh->dim], s[mesh->dim];
    double norm;
    for (int j=0; j<nv; j++) matrix_getcolumn(mesh->vert, vid[j], &x[j]);
    
    functional_veccross(x[0], x[1], cx);
    norm=functional_vecnorm(mesh->dim, cx);
    if (norm<MORPHO_EPS) return false;
    
    functional_veccross(x[1], cx, s);
    matrix_addtocolumn(frc, vid[0], 0.5/norm, s);
    
    functional_veccross(cx, x[0], s);
    matrix_addtocolumn(frc, vid[1], 0.5/norm, s);
    
    return true;
}

FUNCTIONAL_INIT(AreaEnclosed, MESH_GRADE_LINE)
FUNCTIONAL_INTEGRAND(AreaEnclosed, MESH_GRADE_LINE, areaenclosed_integrand)
FUNCTIONAL_GRADIENT(AreaEnclosed, MESH_GRADE_LINE, areaenclosed_gradient, SYMMETRY_ADD)
FUNCTIONAL_TOTAL(AreaEnclosed, MESH_GRADE_LINE, areaenclosed_integrand)

MORPHO_BEGINCLASS(AreaEnclosed)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, AreaEnclosed_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, AreaEnclosed_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, AreaEnclosed_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, AreaEnclosed_total, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * Area
 * ---------------------------------------------- */

/** Calculate area */
bool area_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    double *x[nv], s0[mesh->dim], s1[mesh->dim], cx[mesh->dim];
    for (int j=0; j<nv; j++) matrix_getcolumn(mesh->vert, vid[j], &x[j]);
    
    functional_vecsub(mesh->dim, x[1], x[0], s0);
    functional_vecsub(mesh->dim, x[2], x[1], s1);
    
    functional_veccross(s0, s1, cx);
    
    *out=0.5*functional_vecnorm(mesh->dim, cx);
    return true;
}

/** Calculate gradient */
bool area_gradient(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, objectmatrix *frc) {
    double *x[nv], s0[3], s1[3], s01[3], s010[3], s011[3];
    double norm;
    for (int j=0; j<nv; j++) matrix_getcolumn(mesh->vert, vid[j], &x[j]);
    
    functional_vecsub(mesh->dim, x[1], x[0], s0);
    functional_vecsub(mesh->dim, x[2], x[1], s1);
    
    functional_veccross(s0, s1, s01);
    norm=functional_vecnorm(mesh->dim, s01);
    if (norm<MORPHO_EPS) return false;
    
    functional_veccross(s01, s0, s010);
    functional_veccross(s01, s1, s011);
    
    matrix_addtocolumn(frc, vid[0], 0.5/norm, s011);
    matrix_addtocolumn(frc, vid[2], 0.5/norm, s010);
    
    functional_vecadd(mesh->dim, s010, s011, s0);
    
    matrix_addtocolumn(frc, vid[1], -0.5/norm, s0);
    
    return true;
}

FUNCTIONAL_INIT(Area, MESH_GRADE_AREA)
FUNCTIONAL_INTEGRAND(Area, MESH_GRADE_AREA, area_integrand)
FUNCTIONAL_GRADIENT(Area, MESH_GRADE_AREA, area_gradient, SYMMETRY_ADD)
FUNCTIONAL_TOTAL(Area, MESH_GRADE_AREA, area_integrand)

MORPHO_BEGINCLASS(Area)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, Area_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, Area_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, Area_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, Area_total, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * Enclosed volume
 * ---------------------------------------------- */

/** Calculate enclosed volume */
bool volumeenclosed_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    double *x[nv], cx[mesh->dim];
    for (int j=0; j<nv; j++) matrix_getcolumn(mesh->vert, vid[j], &x[j]);
    
    functional_veccross(x[0], x[1], cx);
    
    *out=fabs(functional_vecdot(mesh->dim, cx, x[2]))/6.0;
    return true;
}

/** Calculate gradient */
bool volumeenclosed_gradient(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, objectmatrix *frc) {
    double *x[nv], cx[mesh->dim], dot;
    for (int j=0; j<nv; j++) matrix_getcolumn(mesh->vert, vid[j], &x[j]);
    
    functional_veccross(x[0], x[1], cx);
    dot=functional_vecdot(mesh->dim, cx, x[2]);
    dot/=fabs(dot);
    
    matrix_addtocolumn(frc, vid[2], dot/6.0, cx);
    
    functional_veccross(x[1], x[2], cx);
    matrix_addtocolumn(frc, vid[0], dot/6.0, cx);
    
    functional_veccross(x[2], x[0], cx);
    matrix_addtocolumn(frc, vid[1], dot/6.0, cx);
    
    return true;
}

FUNCTIONAL_INIT(VolumeEnclosed, MESH_GRADE_AREA)
FUNCTIONAL_INTEGRAND(VolumeEnclosed, MESH_GRADE_AREA, volumeenclosed_integrand)
FUNCTIONAL_GRADIENT(VolumeEnclosed, MESH_GRADE_AREA, volumeenclosed_gradient, SYMMETRY_ADD)
FUNCTIONAL_TOTAL(VolumeEnclosed, MESH_GRADE_AREA, volumeenclosed_integrand)

MORPHO_BEGINCLASS(VolumeEnclosed)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, VolumeEnclosed_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, VolumeEnclosed_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, VolumeEnclosed_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, VolumeEnclosed_total, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * Volume
 * ---------------------------------------------- */

/** Calculate enclosed volume */
bool volume_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    double *x[nv], s10[mesh->dim], s20[mesh->dim], s30[mesh->dim], cx[mesh->dim];
    for (int j=0; j<nv; j++) matrix_getcolumn(mesh->vert, vid[j], &x[j]);
    
    functional_vecsub(mesh->dim, x[1], x[0], s10);
    functional_vecsub(mesh->dim, x[2], x[0], s20);
    functional_vecsub(mesh->dim, x[3], x[0], s30);
    
    functional_veccross(s20, s30, cx);
    
    *out=fabs(functional_vecdot(mesh->dim, s10, cx))/6.0;
    return true;
}

/** Calculate gradient */
bool volume_gradient(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, objectmatrix *frc) {
    double *x[nv], s10[mesh->dim], s20[mesh->dim], s30[mesh->dim];
    double s31[mesh->dim], s21[mesh->dim], cx[mesh->dim], uu;
    for (int j=0; j<nv; j++) matrix_getcolumn(mesh->vert, vid[j], &x[j]);
    
    functional_vecsub(mesh->dim, x[1], x[0], s10);
    functional_vecsub(mesh->dim, x[2], x[0], s20);
    functional_vecsub(mesh->dim, x[3], x[0], s30);
    functional_vecsub(mesh->dim, x[3], x[1], s31);
    functional_vecsub(mesh->dim, x[2], x[1], s21);
    
    functional_veccross(s20, s30, cx);
    uu=functional_vecdot(mesh->dim, s10, cx);
    uu=(uu>0 ? 1.0 : -1.0);
    
    matrix_addtocolumn(frc, vid[1], uu/6.0, cx);
    
    functional_veccross(s31, s21, cx);
    matrix_addtocolumn(frc, vid[0], uu/6.0, cx);
    
    functional_veccross(s30, s10, cx);
    matrix_addtocolumn(frc, vid[2], uu/6.0, cx);
    
    functional_veccross(s10, s20, cx);
    matrix_addtocolumn(frc, vid[3], uu/6.0, cx);
    
    return true;
}

FUNCTIONAL_INIT(Volume, MESH_GRADE_VOLUME)
FUNCTIONAL_INTEGRAND(Volume, MESH_GRADE_VOLUME, volume_integrand)
FUNCTIONAL_GRADIENT(Volume, MESH_GRADE_VOLUME, volume_gradient, SYMMETRY_ADD)
FUNCTIONAL_TOTAL(Volume, MESH_GRADE_VOLUME, volume_integrand)

MORPHO_BEGINCLASS(Volume)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, Volume_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, Volume_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, Volume_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, Volume_total, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * Scalar potential
 * ---------------------------------------------- */

static value scalarpotential_functionproperty;
static value scalarpotential_gradfunctionproperty;

/** Evaluate the scalar potential */
bool scalarpotential_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    double *x;
    value fn = *(value *) ref;
    value args[mesh->dim];
    value ret;
    
    matrix_getcolumn(mesh->vert, id, &x);
    for (int i=0; i<mesh->dim; i++) args[i]=MORPHO_FLOAT(x[i]);
    
    if (morpho_call(v, fn, mesh->dim, args, &ret)) {
        return morpho_valuetofloat(ret, out);
    }
    
    return false;
}

/** Evaluate the gradient of the scalar potential */
bool scalarpotential_gradient(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, objectmatrix *frc) {
    double *x;
    value fn = *(value *) ref;
    value args[mesh->dim];
    value ret;
    
    matrix_getcolumn(mesh->vert, id, &x);
    for (int i=0; i<mesh->dim; i++) args[i]=MORPHO_FLOAT(x[i]);
    
    if (morpho_call(v, fn, mesh->dim, args, &ret)) {
        if (MORPHO_ISMATRIX(ret)) {
            objectmatrix *vf=MORPHO_GETMATRIX(ret);
            
            if (vf->nrows*vf->ncols==frc->nrows) {
                return matrix_addtocolumn(frc, id, 1.0, vf->elements);
            }
        }
    }
    
    return false;
}


/** Initialize a scalar potential */
value ScalarPotential_init(vm *v, int nargs, value *args) {
    objectinstance_setproperty(MORPHO_GETINSTANCE(MORPHO_SELF(args)), functional_gradeproperty, MORPHO_INTEGER(MESH_GRADE_VERTEX));
    
    /* First argument is the potential function */
    if (nargs>0) {
        if (MORPHO_ISCALLABLE(MORPHO_GETARG(args, 0))) {
            objectinstance_setproperty(MORPHO_GETINSTANCE(MORPHO_SELF(args)), scalarpotential_functionproperty, MORPHO_GETARG(args, 0));
        } else morpho_runtimeerror(v, SCALARPOTENTIAL_FNCLLBL);
    }
    /* Second argument is the gradient of the potential function */
    if (nargs>1) {
        if (MORPHO_ISCALLABLE(MORPHO_GETARG(args, 1))) {
            objectinstance_setproperty(MORPHO_GETINSTANCE(MORPHO_SELF(args)), scalarpotential_gradfunctionproperty, MORPHO_GETARG(args, 1));
        } else morpho_runtimeerror(v, SCALARPOTENTIAL_FNCLLBL);
    }
    
    return MORPHO_NIL;
}

/** Integrand function */
value ScalarPotential_integrand(vm *v, int nargs, value *args) {
    functional_mapinfo info;
    value out=MORPHO_NIL;
    
    if (functional_validateargs(v, nargs, args, &info)) {
        value fn;
        if (objectinstance_getproperty(MORPHO_GETINSTANCE(MORPHO_SELF(args)), scalarpotential_functionproperty, &fn)) {
            info.g = MESH_GRADE_VERTEX;
            info.integrand = scalarpotential_integrand;
            info.ref = &fn;
            if (MORPHO_ISCALLABLE(fn)) {
                functional_mapintegrand(v, &info, &out);
            } else morpho_runtimeerror(v, SCALARPOTENTIAL_FNCLLBL);
        } else morpho_runtimeerror(v, VM_OBJECTLACKSPROPERTY, SCALARPOTENTIAL_FUNCTION_PROPERTY);
    }
    if (!MORPHO_ISNIL(out)) morpho_bindobjects(v, 1, &out);
    return out;
}

/** Evaluate a gradient */
value ScalarPotential_gradient(vm *v, int nargs, value *args) {
    functional_mapinfo info;
    value out=MORPHO_NIL;
    
    if (functional_validateargs(v, nargs, args, &info)) {
        value fn;
        // Check if a gradient function is available
        if (objectinstance_getproperty(MORPHO_GETINSTANCE(MORPHO_SELF(args)), scalarpotential_gradfunctionproperty, &fn)) {
            info.g = MESH_GRADE_VERTEX;
            info.grad = scalarpotential_gradient;
            info.ref = &fn;
            if (MORPHO_ISCALLABLE(fn)) {
                functional_mapgradient(v, &info, &out);
            } else morpho_runtimeerror(v, SCALARPOTENTIAL_FNCLLBL);
        } else if (objectinstance_getproperty(MORPHO_GETINSTANCE(MORPHO_SELF(args)), scalarpotential_functionproperty, &fn)) {
            // Otherwise try to use the regular scalar function
            UNREACHABLE("Numerical derivative not implemented");
        } else morpho_runtimeerror(v, VM_OBJECTLACKSPROPERTY, SCALARPOTENTIAL_FUNCTION_PROPERTY);
    }
    if (!MORPHO_ISNIL(out)) morpho_bindobjects(v, 1, &out);
    
    return out;
}

/** Total function */
value ScalarPotential_total(vm *v, int nargs, value *args) {
    functional_mapinfo info;
    value out=MORPHO_NIL;
    
    if (functional_validateargs(v, nargs, args, &info)) {
        value fn;
        if (objectinstance_getproperty(MORPHO_GETINSTANCE(MORPHO_SELF(args)), scalarpotential_functionproperty, &fn)) {
            info.g = MESH_GRADE_VERTEX;
            info.integrand = scalarpotential_integrand;
            info.ref = &fn;
            if (MORPHO_ISCALLABLE(fn)) {
                functional_sumintegrand(v, &info, &out);
            } else morpho_runtimeerror(v, SCALARPOTENTIAL_FNCLLBL);
        } else morpho_runtimeerror(v, VM_OBJECTLACKSPROPERTY, SCALARPOTENTIAL_FUNCTION_PROPERTY);
    }
    return out;
}

MORPHO_BEGINCLASS(ScalarPotential)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, ScalarPotential_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, ScalarPotential_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, ScalarPotential_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, ScalarPotential_total, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * Linear Elasticity
 * ---------------------------------------------- */

static value linearelasticity_referenceproperty;
static value linearelasticity_poissonproperty;

typedef struct {
    objectmesh *refmesh;
    grade grade;
    double lambda; // Lamé coefficients
    double mu;     //
} linearelasticityref;

/** Calculates the Gram matrix */
void linearelasticity_calculategram(objectmatrix *vert, int dim, int nv, int *vid, objectmatrix *gram) {
    int gdim=nv-1; // Dimension of Gram matrix
    double *x[nv], // Positions of vertices
            s[gdim][nv]; // Side vectors
    
    for (int j=0; j<nv; j++) matrix_getcolumn(vert, vid[j], &x[j]); // Get vertices
    for (int j=1; j<nv; j++) functional_vecsub(dim, x[j], x[0], s[j-1]); // u_i = X_i - X_0
    // <u_i, u_j>
    for (int i=0; i<nv-1; i++) for (int j=0; j<nv-1; j++) gram->elements[i+j*gdim]=functional_vecdot(dim, s[i], s[j]);
}

/** Calculate the linear elastic energy */
bool linearelasticity_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    double weight=0.0;
    linearelasticityref *info = (linearelasticityref *) ref;
    int gdim=nv-1; // Dimension of Gram matrix
    
    /* Construct static matrices */
    double gramrefel[gdim*gdim], gramdefel[gdim*gdim], qel[gdim*gdim], rel[gdim*gdim], cgel[gdim*gdim];
    objectmatrix gramref = MORPHO_STATICMATRIX(gramrefel, gdim, gdim); // Gram matrices
    objectmatrix gramdef = MORPHO_STATICMATRIX(gramdefel, gdim, gdim); //
    objectmatrix q = MORPHO_STATICMATRIX(qel, gdim, gdim); // Inverse of Gram in source domain
    objectmatrix r = MORPHO_STATICMATRIX(rel, gdim, gdim); // Intermediate calculations
    objectmatrix cg = MORPHO_STATICMATRIX(cgel, gdim, gdim); // Cauchy-Green strain tensor
    
    linearelasticity_calculategram(info->refmesh->vert, mesh->dim, nv, vid, &gramref);
    linearelasticity_calculategram(mesh->vert, mesh->dim, nv, vid, &gramdef);
    
    if (matrix_inverse(&gramref, &q)!=MATRIX_OK) return false;
    if (matrix_mul(&gramdef, &q, &r)!=MATRIX_OK) return false;
    
    matrix_identity(&cg);
    matrix_scale(&cg, -0.5);
    matrix_accumulate(&cg, 0.5, &r);
    
    double trcg=0.0, trcgcg=0.0;
    matrix_trace(&cg, &trcg);
    
    matrix_mul(&cg, &cg, &r);
    matrix_trace(&r, &trcgcg);
    
    if (!functional_elementsize(v, info->refmesh, info->grade, id, nv, vid, &weight)) return false;
    
    *out=weight*(info->mu*trcgcg + 0.5*info->lambda*trcg*trcg);
    
    return true;
}

/** Prepares the reference structure from the LinearElasticity object's properties */
bool linearelasticity_prepareref(objectinstance *self, linearelasticityref *ref) {
    bool success=false;
    value refmesh=MORPHO_NIL;
    value grade=MORPHO_NIL;
    value poisson=MORPHO_NIL;
    
    if (objectinstance_getproperty(self, linearelasticity_referenceproperty, &refmesh) &&
        objectinstance_getproperty(self, functional_gradeproperty, &grade) &&
        MORPHO_ISINTEGER(grade) &&
        objectinstance_getproperty(self, linearelasticity_poissonproperty, &poisson) &&
        MORPHO_ISNUMBER(poisson)) {
        ref->refmesh=MORPHO_GETMESH(refmesh);
        ref->grade=MORPHO_GETINTEGERVALUE(grade);
        
        double nu = MORPHO_GETFLOATVALUE(poisson);
        
        ref->mu=0.5/(1+nu);
        ref->lambda=nu/(1+nu)/(1-2*nu);
        success=true;
    }
    return success;
}

value LinearElasticity_init(vm *v, int nargs, value *args) {
    objectinstance *self = MORPHO_GETINSTANCE(MORPHO_SELF(args));
    /* First argument is the reference mesh */
    if (nargs>0) {
        if (MORPHO_ISMESH(MORPHO_GETARG(args, 0))) {
            objectinstance_setproperty(self, linearelasticity_referenceproperty, MORPHO_GETARG(args, 0));
            objectmesh *mesh = MORPHO_GETMESH(MORPHO_GETARG(args, 0));
            
            objectinstance_setproperty(self, functional_gradeproperty, MORPHO_INTEGER(mesh_maxgrade(mesh)));
            objectinstance_setproperty(self, linearelasticity_poissonproperty, MORPHO_FLOAT(0.3));
        } else morpho_runtimeerror(v, LINEARELASTICITY_REF);
    } else morpho_runtimeerror(v, LINEARELASTICITY_REF);
    
    /* Second (optional) argument is the grade to act on */
    if (nargs>1) {
        if (MORPHO_ISINTEGER(MORPHO_GETARG(args, 1))) {
            objectinstance_setproperty(MORPHO_GETINSTANCE(MORPHO_SELF(args)), functional_gradeproperty, MORPHO_GETARG(args, 1));
        }
    }
    
    return MORPHO_NIL;
}

/** Integrand function */
value LinearElasticity_integrand(vm *v, int nargs, value *args) {
    functional_mapinfo info;
    linearelasticityref ref;
    value out=MORPHO_NIL;
    
    if (functional_validateargs(v, nargs, args, &info)) {
        if (linearelasticity_prepareref(MORPHO_GETINSTANCE(MORPHO_SELF(args)), &ref)) {
            info.g = ref.grade;
            info.integrand = linearelasticity_integrand;
            info.ref = &ref;
            functional_mapintegrand(v, &info, &out);
        } else morpho_runtimeerror(v, LINEARELASTICITY_PRP);
    }
    if (!MORPHO_ISNIL(out)) morpho_bindobjects(v, 1, &out);
    return out;
}

/** Total function */
value LinearElasticity_total(vm *v, int nargs, value *args) {
    functional_mapinfo info;
    linearelasticityref ref;
    value out=MORPHO_NIL;
    
    if (functional_validateargs(v, nargs, args, &info)) {
        if (linearelasticity_prepareref(MORPHO_GETINSTANCE(MORPHO_SELF(args)), &ref)) {
            info.g = ref.grade;
            info.integrand = linearelasticity_integrand;
            info.ref = &ref;
            functional_sumintegrand(v, &info, &out);
        } else morpho_runtimeerror(v, LINEARELASTICITY_PRP);
    }
    return out;
}

/** Integrand function */
value LinearElasticity_gradient(vm *v, int nargs, value *args) {
    functional_mapinfo info;
    linearelasticityref ref;
    value out=MORPHO_NIL;
    
    if (functional_validateargs(v, nargs, args, &info)) {
        if (linearelasticity_prepareref(MORPHO_GETINSTANCE(MORPHO_SELF(args)), &ref)) {
            info.g = ref.grade;
            info.integrand = linearelasticity_integrand;
            info.ref = &ref;
            info.sym = SYMMETRY_ADD;
            functional_mapnumericalgradient(v, &info, &out);
        } else morpho_runtimeerror(v, LINEARELASTICITY_PRP);
    }
    if (!MORPHO_ISNIL(out)) morpho_bindobjects(v, 1, &out);
    return out;
}

MORPHO_BEGINCLASS(LinearElasticity)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, LinearElasticity_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, LinearElasticity_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, LinearElasticity_total, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, LinearElasticity_gradient, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * Equielement
 * ---------------------------------------------- */

static value equielement_weightproperty;

typedef struct {
    grade grade;
    objectsparse *vtoel; // Connect vertices to elements
    objectsparse *eltov; // Connect elements to vertices
    objectmatrix *weight; // Weight field
    double mean;
} equielementref;

/** Prepares the reference structure from the Equielement object's properties */
bool equielement_prepareref(objectinstance *self, objectmesh *mesh, grade g, objectselection *sel, equielementref *ref) {
    bool success=false;
    value grade=MORPHO_NIL;
    value weight=MORPHO_NIL;
    
    if (objectinstance_getproperty(self, functional_gradeproperty, &grade) &&
        MORPHO_ISINTEGER(grade) ) {
        ref->grade=MORPHO_GETINTEGERVALUE(grade);
        ref->weight=NULL;
        
        int maxgrade=mesh_maxgrade(mesh);
        if (ref->grade<0 || ref->grade>maxgrade) ref->grade = maxgrade;
        
        ref->vtoel=mesh_addconnectivityelement(mesh, ref->grade, 0);
        ref->eltov=mesh_addconnectivityelement(mesh, 0, ref->grade);
        
        if (ref->vtoel && ref->eltov) success=true;
    }
    
    if (objectinstance_getproperty(self, equielement_weightproperty, &weight) &&
        MORPHO_ISMATRIX(weight) ) {
        ref->weight=MORPHO_GETMATRIX(weight);
        if (ref->weight) {
            ref->mean=matrix_sum(ref->weight);
            ref->mean/=ref->weight->ncols;
        }
    }
    
    return success;
}

/** Calculate the linear elastic energy */
bool equielement_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *r, double *out) {
    equielementref *ref = (equielementref *) r;
    int nconn, *conn;
    
    if (sparseccs_getrowindices(&ref->vtoel->ccs, id, &nconn, &conn)) {
        if (nconn==1) { *out = 0; return true; }
        
        double size[nconn], mean=0.0, total=0.0;
        
        for (int i=0; i<nconn; i++) {
            int nv, *vid;
            sparseccs_getrowindices(&ref->eltov->ccs, conn[i], &nv, &vid);
            functional_elementsize(v, mesh, ref->grade, conn[i], nv, vid, &size[i]);
            mean+=size[i];
        }
        
        mean /= ((double) nconn);
        
        if (fabs(mean)<MORPHO_EPS) return false;
        
        /* Now evaluate the functional at this vertex */
        if (!ref->weight || fabs(ref->mean)<MORPHO_EPS) {
            for (unsigned int i=0; i<nconn; i++) total+=(1.0-size[i]/mean)*(1.0-size[i]/mean);
        } else {
            double weight[nconn], wmean=0.0;
            
            for (int i=0; i<nconn; i++) {
                weight[i]=1.0;
                matrix_getelement(ref->weight, 0, conn[i], &weight[i]);
                wmean+=weight[i];
            }
            
            wmean /= ((double) nconn);
            if (fabs(wmean)<MORPHO_EPS) wmean = 1.0;
            
            for (unsigned int i=0; i<nconn; i++) {
                double term = (1.0-weight[i]*size[i]/mean/wmean);
                total+=term*term;
            }
        }
        
        *out = total;
    }
    
    return true;
}

value EquiElement_init(vm *v, int nargs, value *args) {
    objectinstance *self = MORPHO_GETINSTANCE(MORPHO_SELF(args));
    int nfixed;
    value grade=MORPHO_INTEGER(-1);
    value weight=MORPHO_NIL;
    
    if (builtin_options(v, nargs, args, &nfixed, 2, equielement_weightproperty, &weight, functional_gradeproperty, &grade)) {
        objectinstance_setproperty(self, equielement_weightproperty, weight);
        objectinstance_setproperty(self, functional_gradeproperty, grade);
    } else morpho_runtimeerror(v, EQUIELEMENT_ARGS);
    
    return MORPHO_NIL;
}

FUNCTIONAL_METHOD(EquiElement, integrand, MESH_GRADE_VERTEX, equielementref, equielement_prepareref, functional_mapintegrand, equielement_integrand, NULL, EQUIELEMENT_ARGS, SYMMETRY_NONE)

FUNCTIONAL_METHOD(EquiElement, total, MESH_GRADE_VERTEX, equielementref, equielement_prepareref, functional_sumintegrand, equielement_integrand, NULL, EQUIELEMENT_ARGS, SYMMETRY_NONE)

FUNCTIONAL_METHOD(EquiElement, gradient, MESH_GRADE_VERTEX, equielementref, equielement_prepareref, functional_mapnumericalgradient, equielement_integrand, NULL, EQUIELEMENT_ARGS, SYMMETRY_ADD)

MORPHO_BEGINCLASS(EquiElement)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, EquiElement_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, EquiElement_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, EquiElement_total, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, EquiElement_gradient, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* **********************************************************************
 * Curvatures
 * ********************************************************************** */

/* ----------------------------------------------
 * LineCurvatureSq
 * ---------------------------------------------- */

static value curvature_integrandonlyproperty;

typedef struct {
    objectsparse *lineel; // Lines
    objectselection *selection; // Selection
    bool integrandonly; // Output integrated curvature or 'bare' curvature.
} curvatureref;

bool curvature_prepareref(objectinstance *self, objectmesh *mesh, grade g, objectselection *sel, curvatureref *ref) {
    bool success = true;
    
    ref->selection=sel;
    
    ref->lineel = mesh_getconnectivityelement(mesh, MESH_GRADE_VERTEX, MESH_GRADE_LINE);
    if (ref->lineel) success=sparse_checkformat(ref->lineel, SPARSE_CCS, true, false);
    
    if (success) {
        objectsparse *s = mesh_getconnectivityelement(mesh, MESH_GRADE_LINE, MESH_GRADE_VERTEX);
        if (!s) s=mesh_addconnectivityelement(mesh, MESH_GRADE_LINE, MESH_GRADE_VERTEX);
        success=s;
    }
    
    if (success) {
        value integrandonly=MORPHO_FALSE;
        objectinstance_getproperty(self, curvature_integrandonlyproperty, &integrandonly);
        ref->integrandonly=MORPHO_ISTRUE(integrandonly);
    }
    
    return success;
}

/** Finds the points that a point depends on  */
bool linecurvsq_dependencies(functional_mapinfo *info, elementid id, varray_elementid *out) {
    objectmesh *mesh = info->mesh;
    curvatureref *cref = info->ref;
    bool success=false;
    varray_elementid nbrs;
    varray_elementidinit(&nbrs);
    
    if (mesh_findneighbors(mesh, MESH_GRADE_VERTEX, id, MESH_GRADE_LINE, &nbrs)>0) {
        for (unsigned int i=0; i<nbrs.count; i++) {
            int nentries, *entries; // Get the vertices for this edge
            if (!sparseccs_getrowindices(&cref->lineel->ccs, nbrs.data[i], &nentries, &entries)) goto linecurvsq_dependencies_cleanup;
            for (unsigned int j=0; j<nentries; j++) {
                if (entries[j]==id) continue;
                varray_elementidwrite(out, entries[j]);
            }
        }
    }
    success=true;
    /*printf("Vertex %u: ", id);
    for (int k=0; k<out->count; k++) printf("%u ", out->data[k]);
    printf("\n");*/
    
linecurvsq_dependencies_cleanup:
    varray_elementidclear(&nbrs);
    
    return success;
}

/** Calculate the integral of the curvature squared  */
bool linecurvsq_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    curvatureref *cref = (curvatureref *) ref;
    double result = 0.0;
    varray_elementid nbrs;
    varray_elementid synid;
    varray_elementidinit(&nbrs);
    varray_elementidinit(&synid);
    
    double s0[mesh->dim], s1[mesh->dim], *s[2] = { s0, s1}, sgn=-1.0;
    
    if (mesh_findneighbors(mesh, MESH_GRADE_VERTEX, id, MESH_GRADE_LINE, &nbrs)>0 &&
        mesh_getsynonyms(mesh, MESH_GRADE_VERTEX, id, &synid)) {
        if (nbrs.count<2) goto linecurvsq_integrand_cleanup; 
        
        for (unsigned int i=0; i<nbrs.count; i++) {
            int nentries, *entries; // Get the vertices for this edge
            if (!sparseccs_getrowindices(&cref->lineel->ccs, nbrs.data[i], &nentries, &entries)) break;
            
            double *x0, *x1;
            if (mesh_getvertexcoordinatesaslist(mesh, entries[0], &x0) &&
                mesh_getvertexcoordinatesaslist(mesh, entries[1], &x1)) {
                functional_vecsub(mesh->dim, x0, x1, s[i]);
            }
            if (!(entries[0]==id || functional_inlist(&synid, entries[0]))) sgn*=-1;
        }
        
        double s0s0=functional_vecdot(mesh->dim, s0, s0),
               s0s1=functional_vecdot(mesh->dim, s0, s1),
               s1s1=functional_vecdot(mesh->dim, s1, s1);
        
        s0s0=sqrt(s0s0); s1s1=sqrt(s1s1);
        
        if (s0s0<MORPHO_EPS || s1s1<MORPHO_EPS) return false;

        double u=sgn*s0s1/s0s0/s1s1,
               len=0.5*(s0s0+s1s1);

        if (u<1) u=acos(u); else u=0;
        
        result = u*u/len;
        if (cref->integrandonly) result /= len; // Get the bare curvature.
    }
    
linecurvsq_integrand_cleanup:
    
    *out = result;
    varray_elementidclear(&nbrs);
    varray_elementidclear(&synid);
    
    return true;
}

FUNCTIONAL_INIT(LineCurvatureSq, MESH_GRADE_VERTEX)
FUNCTIONAL_METHOD(LineCurvatureSq, integrand, MESH_GRADE_VERTEX, curvatureref, curvature_prepareref, functional_mapintegrand, linecurvsq_integrand, NULL, FUNCTIONAL_ARGS, SYMMETRY_NONE)
FUNCTIONAL_METHOD(LineCurvatureSq, total, MESH_GRADE_VERTEX, curvatureref, curvature_prepareref, functional_sumintegrand, linecurvsq_integrand, NULL, FUNCTIONAL_ARGS, SYMMETRY_NONE)
FUNCTIONAL_METHOD(LineCurvatureSq, gradient, MESH_GRADE_VERTEX, curvatureref, curvature_prepareref, functional_mapnumericalgradient, linecurvsq_integrand, linecurvsq_dependencies, FUNCTIONAL_ARGS, SYMMETRY_ADD)

MORPHO_BEGINCLASS(LineCurvatureSq)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, LineCurvatureSq_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, LineCurvatureSq_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, LineCurvatureSq_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, LineCurvatureSq_total, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * LineTorsionSq
 * ---------------------------------------------- */

/** Return a list of vertices that an element depends on  */
bool linetorsionsq_dependencies(functional_mapinfo *info, elementid id, varray_elementid *out) {
    objectmesh *mesh = info->mesh;
    curvatureref *cref = info->ref;
    bool success=false;
    varray_elementid nbrs;
    varray_elementid synid;
    
    varray_elementidinit(&nbrs);
    varray_elementidinit(&synid);
    
    if (mesh_findneighbors(mesh, MESH_GRADE_LINE, id, MESH_GRADE_LINE, &nbrs)>0) {
        for (unsigned int i=0; i<nbrs.count; i++) {
            int nentries, *entries; // Get the vertices for this edge
            if (!sparseccs_getrowindices(&cref->lineel->ccs, nbrs.data[i], &nentries, &entries)) goto linetorsionsq_dependencies_cleanup;
            for (unsigned int j=0; j<nentries; j++) {
                varray_elementidwriteunique(out, entries[j]);
            }
        }
    }
    success=true;
    
linetorsionsq_dependencies_cleanup:
    varray_elementidclear(&nbrs);
    varray_elementidclear(&synid);
    
    return success;
}


/** Calculate the integral of the torsion squared  */
bool linetorsionsq_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    curvatureref *cref = (curvatureref *) ref;
    int tmpi; elementid tmpid;
    bool success=false;
    
    //double result = 0.0;
    varray_elementid nbrs;
    varray_elementid synid;
    varray_elementidinit(&nbrs);
    varray_elementidinit(&synid);
    elementid vlist[6]; // List of vertices in order  n
    int type[6];
    for (unsigned int i=0; i<6; i++) type[i]=-1;
    
    /* We want an ordered list of vertex indices:
     *               v the element
     *    0 --- 1/2 --- 3/4 --- 5
     * Where 1/2 and 3/4 are the same vertex, but could have different indices due to symmetries */
     vlist[2] = vid[0]; vlist[3] = vid[1]; // Copy the current element into place
    
    /* First identify neighbors and get the vertex ids for each element */
    if (mesh_findneighbors(mesh, MESH_GRADE_LINE, id, MESH_GRADE_LINE, &nbrs)>0) {
        if (nbrs.count<2) {
            *out = 0; success=true;
            goto linecurvsq_torsion_cleanup;
        }
        
        for (unsigned int i=0; i<nbrs.count; i++) {
            int nentries, *entries; // Get the vertices for this edge
            if (!sparseccs_getrowindices(&cref->lineel->ccs, nbrs.data[i], &nentries, &entries)) goto linecurvsq_torsion_cleanup;
            for (unsigned int j=0; j<nentries; j++) { // Copy the vertexids
                vlist[4*i+j] = entries[j];
            }
        }
    }
    
    /* The vertex ids are not yet in the right order. Let's identify which vertex is which */
    for (int i=0; i<2; i++) {
        if (mesh_getsynonyms(mesh, 0, vid[i], &synid)) {
            for (int j=0; j<6; j++) if (vlist[j]==vid[i] || functional_inlist(&synid, vlist[j])) type[j]=i;
        }
    }
    /* The type array now contains either 0,1 depending on which vertex we have, or -1 if the vertex is not a synonym for the element's vertices */
#define SWAP(var, i, j, tmp) { tmp=var[i]; var[i]=var[j]; var[j]=tmp; }
    if (type[0]==1 || type[1]==1) { // Make sure the first segment corresponds to the first vertex
        SWAP(vlist, 0, 4, tmpid); SWAP(vlist, 1, 5, tmpid);
        SWAP(type, 0, 4, tmpi); SWAP(type, 1, 5, tmpi);
    }
    
    if (type[1]==-1) { // Check order of first segment
        SWAP(vlist, 0, 1, tmpid);
        SWAP(type, 0, 1, tmpi);
    }
    
    if (type[4]==-1) { // Check order of first segment
        SWAP(vlist, 4, 5, tmpid);
        SWAP(type, 4, 5, tmpi);
    }
#undef SWAP
    
    /* We now have an ordered list of vertices.
       Get the vertex positions */
    double *x[6];
    for (int i=0; i<6; i++) matrix_getcolumn(mesh->vert, vlist[i], &x[i]);
    
    double A[3], B[3], C[3], crossAB[3], crossBC[3];
    functional_vecsub(3, x[1], x[0], A);
    functional_vecsub(3, x[3], x[2], B);
    functional_vecsub(3, x[5], x[4], C);
    
    functional_veccross(A, B, crossAB);
    functional_veccross(B, C, crossBC);
    
    double normB=functional_vecnorm(3, B),
           normAB=functional_vecnorm(3, crossAB),
           normBC=functional_vecnorm(3, crossBC);
    
    double S = functional_vecdot(3, A, crossBC)*normB;
    if (normAB>MORPHO_EPS) S/=normAB;
    if (normBC>MORPHO_EPS) S/=normBC;
    
    S=asin(S);
    *out=S*S/normB;
    success=true;
    
linecurvsq_torsion_cleanup:
    varray_elementidclear(&nbrs);
    varray_elementidclear(&synid);
    
    return success;
}

FUNCTIONAL_INIT(LineTorsionSq, MESH_GRADE_LINE)
FUNCTIONAL_METHOD(LineTorsionSq, integrand, MESH_GRADE_LINE, curvatureref, curvature_prepareref, functional_mapintegrand, linetorsionsq_integrand, NULL, FUNCTIONAL_ARGS, SYMMETRY_NONE)
FUNCTIONAL_METHOD(LineTorsionSq, total, MESH_GRADE_LINE, curvatureref, curvature_prepareref, functional_sumintegrand, linetorsionsq_integrand, NULL, FUNCTIONAL_ARGS, SYMMETRY_NONE)
FUNCTIONAL_METHOD(LineTorsionSq, gradient, MESH_GRADE_LINE, curvatureref, curvature_prepareref, functional_mapnumericalgradient, linetorsionsq_integrand, linetorsionsq_dependencies, FUNCTIONAL_ARGS, SYMMETRY_ADD)

MORPHO_BEGINCLASS(LineTorsionSq)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, LineTorsionSq_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, LineTorsionSq_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, LineTorsionSq_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, LineTorsionSq_total, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * MeanCurvatureSq
 * ---------------------------------------------- */

typedef struct {
    objectsparse *areael; // Areas
    objectselection *selection; // Selection
    bool integrandonly; // Output integrated curvature or 'bare' curvature.
} areacurvatureref;

bool areacurvature_prepareref(objectinstance *self, objectmesh *mesh, grade g, objectselection *sel, areacurvatureref *ref) {
    bool success = true;
    
    ref->selection=sel;
    
    ref->areael = mesh_getconnectivityelement(mesh, MESH_GRADE_VERTEX, MESH_GRADE_AREA);
    if (ref->areael) success=sparse_checkformat(ref->areael, SPARSE_CCS, true, false);
    
    if (success) {
        objectsparse *s = mesh_getconnectivityelement(mesh, MESH_GRADE_AREA, MESH_GRADE_VERTEX);
        if (!s) s=mesh_addconnectivityelement(mesh, MESH_GRADE_AREA, MESH_GRADE_VERTEX);
        success=s;
    }
    
    if (success) {
        value integrandonly=MORPHO_FALSE;
        objectinstance_getproperty(self, curvature_integrandonlyproperty, &integrandonly);
        ref->integrandonly=MORPHO_ISTRUE(integrandonly);
    }
    
    return success;
}

/** Return a list of vertices that an element depends on  */
bool meancurvaturesq_dependencies(functional_mapinfo *info, elementid id, varray_elementid *out) {
    objectmesh *mesh = info->mesh;
    areacurvatureref *cref = info->ref;
    bool success=false;
    varray_elementid nbrs;
    varray_elementid synid;
    
    varray_elementidinit(&nbrs);
    varray_elementidinit(&synid);
    
    mesh_getsynonyms(mesh, MESH_GRADE_VERTEX, id, &synid);
    varray_elementidwriteunique(&synid, id);
    
    /* Loop over synonyms of the element id */
    mesh_findneighbors(mesh, MESH_GRADE_VERTEX, id, MESH_GRADE_AREA, &nbrs);
    
    for (unsigned int i=0; i<nbrs.count; i++) { /* Loop over adjacent triangles */
        int nvert, *vids;
        if (!sparseccs_getrowindices(&cref->areael->ccs, nbrs.data[i], &nvert, &vids)) goto meancurvsq_dependencies_cleanup;
        
        for (unsigned int j=0; j<nvert; j++) {
            if (vids[j]==id) continue;
            varray_elementidwriteunique(out, vids[j]);
        }
    }
    success=true;
    
meancurvsq_dependencies_cleanup:
    varray_elementidclear(&nbrs);
    varray_elementidclear(&synid);
    
    return success;
}

/** Orders the vertices in the list vids so that the vertex in synid is first */
bool curvature_ordervertices(varray_elementid *synid, int nv, int *vids) {
    int posn=-1;
    for (unsigned int i=0; i<nv && posn<0; i++) {
        for (unsigned int k=0; k<synid->count; k++) if (synid->data[k]==vids[i]) { posn = i; break; }
    }
    
    if (posn>0) { // If the desired vertex isn't in first position, move it there.
        int tmp=vids[posn];
        vids[posn]=vids[0]; vids[0]=tmp;
    }
    
    return (posn>=0);
}

/** Calculate the integral of the mean curvature squared  */
bool meancurvaturesq_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    areacurvatureref *cref = (areacurvatureref *) ref;
    double areasum = 0;
    bool success=false;
    
    varray_elementid nbrs;
    varray_elementid synid;
    varray_elementidinit(&nbrs);
    varray_elementidinit(&synid);
    
    mesh_getsynonyms(mesh, MESH_GRADE_VERTEX, id, &synid);
    varray_elementidwriteunique(&synid, id);
    
    double frc[mesh->dim]; // This will hold the total force due to the triangles present
    for (unsigned int i=0; i<mesh->dim; i++) frc[i]=0.0;
    
    mesh_findneighbors(mesh, MESH_GRADE_VERTEX, id, MESH_GRADE_AREA, &nbrs);
    
    for (unsigned int i=0; i<nbrs.count; i++) { /* Loop over adjacent triangles */
        int nvert, *vids;
        if (!sparseccs_getrowindices(&cref->areael->ccs, nbrs.data[i], &nvert, &vids)) goto meancurvsq_cleanup;
        
        /* Order the vertices */
        if (!curvature_ordervertices(&synid, nvert, vids)) goto meancurvsq_cleanup;
        
        double *x[3], s0[3], s1[3], s01[3], s101[3];
        double norm;
        for (int j=0; j<3; j++) matrix_getcolumn(mesh->vert, vids[j], &x[j]);
        
        /* s0 = x1-x0; s1 = x2-x1 */
        functional_vecsub(mesh->dim, x[1], x[0], s0);
        functional_vecsub(mesh->dim, x[2], x[1], s1);
        
        /* F(v0) = (s1 x s0 x s1)/|s0 x x1|/2 */
        functional_veccross(s0, s1, s01);
        norm=functional_vecnorm(mesh->dim, s01);
        if (norm<MORPHO_EPS) goto meancurvsq_cleanup;
        
        areasum+=norm/2;
        functional_veccross(s1, s01, s101);
        
        functional_vecaddscale(mesh->dim, frc, 0.5/norm, s101, frc);
    }
    
    *out = functional_vecdot(mesh->dim, frc, frc)/(areasum/3.0)/4.0;
    if (cref->integrandonly) *out /= (areasum/3.0);
    success=true;
    
meancurvsq_cleanup:
    varray_elementidclear(&nbrs);
    varray_elementidclear(&synid);
    
    return success;
}

FUNCTIONAL_INIT(MeanCurvatureSq, MESH_GRADE_VERTEX)
FUNCTIONAL_METHOD(MeanCurvatureSq, integrand, MESH_GRADE_VERTEX, areacurvatureref, areacurvature_prepareref, functional_mapintegrand, meancurvaturesq_integrand, NULL, FUNCTIONAL_ARGS, SYMMETRY_NONE)
FUNCTIONAL_METHOD(MeanCurvatureSq, total, MESH_GRADE_VERTEX, areacurvatureref, areacurvature_prepareref, functional_sumintegrand, meancurvaturesq_integrand, NULL, FUNCTIONAL_ARGS, SYMMETRY_NONE)
FUNCTIONAL_METHOD(MeanCurvatureSq, gradient, MESH_GRADE_VERTEX, areacurvatureref, areacurvature_prepareref, functional_mapnumericalgradient, meancurvaturesq_integrand, meancurvaturesq_dependencies, FUNCTIONAL_ARGS, SYMMETRY_ADD)

MORPHO_BEGINCLASS(MeanCurvatureSq)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, MeanCurvatureSq_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, MeanCurvatureSq_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, MeanCurvatureSq_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, MeanCurvatureSq_total, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * GaussCurvature
 * ---------------------------------------------- */

/** Calculate the integral of the gaussian curvature  */
bool gausscurvature_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    areacurvatureref *cref = (areacurvatureref *) ref;
    double anglesum = 0, areasum = 0;
    bool success=false;
    
    varray_elementid nbrs;
    varray_elementid synid;
    varray_elementidinit(&nbrs);
    varray_elementidinit(&synid);
    
    mesh_getsynonyms(mesh, MESH_GRADE_VERTEX, id, &synid);
    varray_elementidwriteunique(&synid, id);
    
    double frc[mesh->dim]; // This will hold the total force due to the triangles present
    for (unsigned int i=0; i<mesh->dim; i++) frc[i]=0.0;
    
    mesh_findneighbors(mesh, MESH_GRADE_VERTEX, id, MESH_GRADE_AREA, &nbrs);
    
    for (unsigned int i=0; i<nbrs.count; i++) { /* Loop over adjacent triangles */
        int nvert, *vids;
        if (!sparseccs_getrowindices(&cref->areael->ccs, nbrs.data[i], &nvert, &vids)) goto gausscurv_cleanup;
        
        /* Order the vertices */
        if (!curvature_ordervertices(&synid, nvert, vids)) goto gausscurv_cleanup;
        
        double *x[3], s0[3], s1[3], s01[3];
        for (int j=0; j<3; j++) matrix_getcolumn(mesh->vert, vids[j], &x[j]);
        
        /* s0 = x1-x0; s1 = x2-x0 */
        functional_vecsub(mesh->dim, x[1], x[0], s0);
        functional_vecsub(mesh->dim, x[2], x[0], s1);
        
        functional_veccross(s0, s1, s01);
        double area = functional_vecnorm(mesh->dim, s01);
        anglesum+=atan2(area, functional_vecdot(mesh->dim, s0, s1));
        
        areasum+=area/2;
    }

    *out = 2*M_PI-anglesum;
    if (cref->integrandonly) *out /= (areasum/3.0);
    success=true;
    
gausscurv_cleanup:
    varray_elementidclear(&nbrs);
    varray_elementidclear(&synid);
    
    return success;
}

FUNCTIONAL_INIT(GaussCurvature, MESH_GRADE_VERTEX)
FUNCTIONAL_METHOD(GaussCurvature, integrand, MESH_GRADE_VERTEX, areacurvatureref, areacurvature_prepareref, functional_mapintegrand, gausscurvature_integrand, NULL, FUNCTIONAL_ARGS, SYMMETRY_NONE)
FUNCTIONAL_METHOD(GaussCurvature, total, MESH_GRADE_VERTEX, areacurvatureref, areacurvature_prepareref, functional_sumintegrand, gausscurvature_integrand, NULL, FUNCTIONAL_ARGS, SYMMETRY_NONE)
FUNCTIONAL_METHOD(GaussCurvature, gradient, MESH_GRADE_VERTEX, areacurvatureref, areacurvature_prepareref, functional_mapnumericalgradient, gausscurvature_integrand, meancurvaturesq_dependencies, FUNCTIONAL_ARGS, SYMMETRY_ADD)

MORPHO_BEGINCLASS(GaussCurvature)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, GaussCurvature_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, GaussCurvature_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, GaussCurvature_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, GaussCurvature_total, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* **********************************************************************
 * Fields
 * ********************************************************************** */

typedef struct {
    objectfield *field;
} fieldref;

/* ----------------------------------------------
 * GradSq
 * ---------------------------------------------- */

bool gradsq_computeperpendicular(unsigned int n, double *s1, double *s2, double *out) {
    double s1s2, s2s2, sout;
    
    /* Compute s1 - (s1.s2) s2 / (s2.2) */
    s1s2 = functional_vecdot(n, s1, s2);
    s2s2 = functional_vecdot(n, s2, s2);
    if (fabs(s2s2)<MORPHO_EPS) return false; // Check for side of zero weight
    
    double temp[n];
    functional_vecscale(n, s1s2/s2s2, s2, temp);
    functional_vecsub(n, s1, temp, out);
    
    /* Scale by 1/|t|^2 */
    sout = functional_vecnorm(n, out);
    if (fabs(sout)<MORPHO_EPS) return false; // Check for side of zero weight
    
    functional_vecscale(n, 1/(sout*sout), out, out);
    return true;
}

/** Evaluates the gradient of a field quantity
 @param[in] mesh - object to use
 @param[in] field - field to compute gradient of
 @param[in] nv - number of vertices
 @param[in] vid - vertex ids
 @param[out] out - should be field->psize * mesh->dim units of storage */
bool gradsq_evaluategradient(objectmesh *mesh, objectfield *field, int nv, int *vid, double *out) {
    double *f[nv]; // Field value lists
    double *x[nv]; // Vertex coordinates
    unsigned int nentries=0;
    
    // Get field values and vertex coordinates
    for (unsigned int i=0; i<nv; i++) {
        if (!mesh_getvertexcoordinatesaslist(mesh, vid[i], &x[i])) return false;
        if (!field_getelementaslist(field, MESH_GRADE_VERTEX, vid[i], 0, &nentries, &f[i])) return false;
    }
    
    double s[3][mesh->dim], t[3][mesh->dim];
    
    /* Vector sides */
    functional_vecsub(mesh->dim, x[1], x[0], s[0]);
    functional_vecsub(mesh->dim, x[2], x[1], s[1]);
    functional_vecsub(mesh->dim, x[0], x[2], s[2]);
    
    /* Perpendicular vectors */
    gradsq_computeperpendicular(mesh->dim, s[2], s[1], t[0]);
    gradsq_computeperpendicular(mesh->dim, s[0], s[2], t[1]);
    gradsq_computeperpendicular(mesh->dim, s[1], s[0], t[2]);
    
    /* Compute the gradient */
    for (unsigned int i=0; i<mesh->dim*nentries; i++) out[i]=0;
    for (unsigned int j=0; j<mesh->dim; j++) {
        for (unsigned int i=0; i<nentries; i++) {
            functional_vecaddscale(mesh->dim, &out[i*mesh->dim], f[j][i], t[j], &out[i*mesh->dim]);
        }
    }
    
    return true;
}

/** Prepares the gradsq reference */
bool gradsq_prepareref(objectinstance *self, objectmesh *mesh, grade g, objectselection *sel, fieldref *ref) {
    bool success=false;
    value field=MORPHO_NIL;
    
    if (objectinstance_getproperty(self, functional_fieldproperty, &field) &&
        MORPHO_ISFIELD(field)) {
        ref->field=MORPHO_GETFIELD(field);
        success=true;
    }
    return success;
}

/** Calculate the |grad q|^2 energy */
bool gradsq_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    fieldref *eref = ref;
    double size=0; // Length area or volume of the element
    double grad[eref->field->psize*mesh->dim];
    
    if (!functional_elementsize(v, mesh, MESH_GRADE_AREA, id, nv, vid, &size)) return false;
    
    if (!gradsq_evaluategradient(mesh, eref->field, nv, vid, grad)) return false;
    
    double gradnrm=functional_vecnorm(eref->field->psize*mesh->dim, grad);
    *out = gradnrm*gradnrm*size;
    
    return true;
}

/** Initialize a GradSq object */
value GradSq_init(vm *v, int nargs, value *args) {
    objectinstance *self = MORPHO_GETINSTANCE(MORPHO_SELF(args));
    
    if (nargs==1 && MORPHO_ISFIELD(MORPHO_GETARG(args, 0))) {
        objectinstance_setproperty(self, functional_fieldproperty, MORPHO_GETARG(args, 0));
    } else morpho_runtimeerror(v, VM_INVALIDARGS);
    
    return MORPHO_NIL;
}

FUNCTIONAL_METHOD(GradSq, integrand, MESH_GRADE_AREA, fieldref, gradsq_prepareref, functional_mapintegrand, gradsq_integrand, NULL, GRADSQ_ARGS, SYMMETRY_NONE);

FUNCTIONAL_METHOD(GradSq, total, MESH_GRADE_AREA, fieldref, gradsq_prepareref, functional_sumintegrand, gradsq_integrand, NULL, GRADSQ_ARGS, SYMMETRY_NONE);

FUNCTIONAL_METHOD(GradSq, gradient, MESH_GRADE_AREA, fieldref, gradsq_prepareref, functional_mapnumericalgradient, gradsq_integrand, NULL, GRADSQ_ARGS, SYMMETRY_ADD);

value GradSq_fieldgradient(vm *v, int nargs, value *args) {
    functional_mapinfo info;
    fieldref ref;
    value out=MORPHO_NIL;
    
    if (functional_validateargs(v, nargs, args, &info)) {
        if (gradsq_prepareref(MORPHO_GETINSTANCE(MORPHO_SELF(args)), info.mesh, MESH_GRADE_AREA, info.sel, &ref)) {
            info.g = MESH_GRADE_AREA;
            info.field = ref.field;
            info.integrand = gradsq_integrand;
            info.ref = &ref;
            functional_mapnumericalfieldgradient(v, &info, &out);
        } else morpho_runtimeerror(v, GRADSQ_ARGS);
    }
    if (!MORPHO_ISNIL(out)) morpho_bindobjects(v, 1, &out);
    return out;
}

MORPHO_BEGINCLASS(GradSq)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, GradSq_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, GradSq_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, GradSq_total, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, GradSq_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_FIELDGRADIENT_METHOD, GradSq_fieldgradient, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * Nematic
 * ---------------------------------------------- */

static value nematic_ksplayproperty;
static value nematic_ktwistproperty;
static value nematic_kbendproperty;
static value nematic_pitchproperty;

typedef struct {
    double ksplay,ktwist,kbend,pitch;
    bool haspitch;
    objectfield *field;
} nematicref;

/** Prepares the nematic reference */
bool nematic_prepareref(objectinstance *self, objectmesh *mesh, grade g, objectselection *sel,  nematicref *ref) {
    bool success=false;
    value field=MORPHO_NIL;
    value val=MORPHO_NIL;
    ref->ksplay=1.0; ref->ktwist=1.0; ref->kbend=1.0; ref->pitch=0.0;
    ref->haspitch=false;
    
    if (objectinstance_getproperty(self, functional_fieldproperty, &field) &&
        MORPHO_ISFIELD(field)) {
        ref->field=MORPHO_GETFIELD(field);
        success=true;
    }
    if (objectinstance_getproperty(self, nematic_ksplayproperty, &val) && MORPHO_ISNUMBER(val)) {
        morpho_valuetofloat(val, &ref->ksplay);
    }
    if (objectinstance_getproperty(self, nematic_ktwistproperty, &val) && MORPHO_ISNUMBER(val)) {
        morpho_valuetofloat(val, &ref->ktwist);
    }
    if (objectinstance_getproperty(self, nematic_kbendproperty, &val) && MORPHO_ISNUMBER(val)) {
        morpho_valuetofloat(val, &ref->kbend);
    }
    if (objectinstance_getproperty(self, nematic_pitchproperty, &val) && MORPHO_ISNUMBER(val)) {
        morpho_valuetofloat(val, &ref->pitch);
        ref->haspitch=true;
    }
    return success;
}

/* Integrates two linear functions with values at vertices f[0]...f[2] and g[0]...g[2] */
double nematic_bcint(double *f, double *g) {
    return (f[0]*(2*g[0]+g[1]+g[2]) + f[1]*(g[0]+2*g[1]+g[2]) + f[2]*(g[0]+g[1]+2*g[2]))/12;
}

/* Integrates a linear vector function with values at vertices f[0]...f[2] */
double nematic_bcint1(double *f) {
    return (f[0] + f[1] + f[2])/3;
}

/** Calculate the nematic energy */
bool nematic_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    nematicref *eref = ref;
    double size=0; // Length area or volume of the element
    double gradnn[eref->field->psize*mesh->dim];
    double divnn, curlnn[mesh->dim];
    
    if (!functional_elementsize(v, mesh, MESH_GRADE_AREA, id, nv, vid, &size)) return false;
    
    // Get nematic director components
    double *nn[nv]; // Field value lists
    unsigned int nentries=0;
    for (unsigned int i=0; i<nv; i++) {
        if (!field_getelementaslist(eref->field, MESH_GRADE_VERTEX, vid[i], 0, &nentries, &nn[i])) return false;
    }
    
    // Evaluate gradients of the director
    if (!gradsq_evaluategradient(mesh, eref->field, nv, vid, gradnn)) return false;
    // Output of this is the matrix:
    // [ nx,x ny,x nz,x ] [ 0 3 6 ] <- indices
    // [ nx,y ny,y nz,y ] [ 1 4 7 ]
    // [ nx,z ny,z nz,z ] [ 2 5 8 ]
    objectmatrix gradnnmat = MORPHO_STATICMATRIX(gradnn, mesh->dim, mesh->dim);
    
    matrix_trace(&gradnnmat, &divnn);
    curlnn[0]=gradnn[7]-gradnn[5]; // nz,y - ny,z
    curlnn[1]=gradnn[2]-gradnn[6]; // nx,z - nz,x
    curlnn[2]=gradnn[3]-gradnn[1]; // ny,x - nx,y
    
    /* From components of the curl, construct the coefficients that go in front of integrals of
           nx^2, ny^2, nz^2, nx*ny, ny*nz, and nz*nx over the element. */
    double ctwst[6] = { curlnn[0]*curlnn[0], curlnn[1]*curlnn[1], curlnn[2]*curlnn[2],
                        2*curlnn[0]*curlnn[1], 2*curlnn[1]*curlnn[2], 2*curlnn[2]*curlnn[0]};

    double cbnd[6] = { ctwst[1] + ctwst[2], ctwst[0] + ctwst[2], ctwst[0] + ctwst[1],
                       -ctwst[3], -ctwst[4], -ctwst[5] };
    
    /* Calculate integrals of nx^2, ny^2, nz^2, nx*ny, ny*nz, and nz*nx over the element */
    double nnt[mesh->dim][nv]; // The transpose of nn
    for (unsigned int i=0; i<nv; i++)
        for (unsigned int j=0; j<mesh->dim; j++) nnt[j][i]=nn[i][j];
    
    double integrals[] = {  nematic_bcint(nnt[0], nnt[0]),
                            nematic_bcint(nnt[1], nnt[1]),
                            nematic_bcint(nnt[2], nnt[2]),
                            nematic_bcint(nnt[0], nnt[1]),
                            nematic_bcint(nnt[1], nnt[2]),
                            nematic_bcint(nnt[2], nnt[0])
    };
    
    /* Now we can calculate the components of splay, twist and bend */
    double splay=0.0, twist=0.0, bend=0.0, chol=0.0;

    /* Evaluate the three contributions to the integral */
    splay = 0.5*eref->ksplay*size*divnn*divnn;
    for (unsigned int i=0; i<6; i++) {
        twist += ctwst[i]*integrals[i];
        bend += cbnd[i]*integrals[i];
    }
    twist *= 0.5*eref->ktwist*size;
    bend *= 0.5*eref->kbend*size;
    
    if (eref->haspitch) {
        /* Cholesteric terms: 0.5 * k22 * [- 2 q (cx <nx> + cy <ny> + cz <nz>) + q^2] */
        for (unsigned i=0; i<3; i++) {
            chol += -2*curlnn[i]*nematic_bcint1(nnt[i])*eref->pitch;
        }
        chol += (eref->pitch*eref->pitch);
        chol *= 0.5*eref->ktwist*size;
    }
    
    *out = splay+twist+bend+chol;
    
    return true;
}

/** Initialize a Nematic object */
value Nematic_init(vm *v, int nargs, value *args) {
    objectinstance *self = MORPHO_GETINSTANCE(MORPHO_SELF(args));

    int nfixed=nargs;
    value ksplay=MORPHO_FLOAT(1.0),
          ktwist=MORPHO_FLOAT(1.0),
          kbend=MORPHO_FLOAT(1.0);
    value pitch=MORPHO_NIL;
    
    if (builtin_options(v, nargs, args, &nfixed, 4,
                        nematic_ksplayproperty, &ksplay,
                        nematic_ktwistproperty, &ktwist,
                        nematic_kbendproperty, &kbend,
                        nematic_pitchproperty, &pitch)) {
        objectinstance_setproperty(self, nematic_ksplayproperty, ksplay);
        objectinstance_setproperty(self, nematic_ktwistproperty, ktwist);
        objectinstance_setproperty(self, nematic_kbendproperty, kbend);
        objectinstance_setproperty(self, nematic_pitchproperty, pitch);
    } else morpho_runtimeerror(v, NEMATIC_ARGS);
    
    if (nfixed==1 && MORPHO_ISFIELD(MORPHO_GETARG(args, 0))) {
        objectinstance_setproperty(self, functional_fieldproperty, MORPHO_GETARG(args, 0));
    } else morpho_runtimeerror(v, NEMATIC_ARGS);
    
    return MORPHO_NIL;
}

FUNCTIONAL_METHOD(Nematic, integrand, MESH_GRADE_AREA, nematicref, nematic_prepareref, functional_mapintegrand, nematic_integrand, NULL, NEMATIC_ARGS, SYMMETRY_NONE);

FUNCTIONAL_METHOD(Nematic, total, MESH_GRADE_AREA, nematicref, nematic_prepareref, functional_sumintegrand, nematic_integrand, NULL, NEMATIC_ARGS, SYMMETRY_NONE);

FUNCTIONAL_METHOD(Nematic, gradient, MESH_GRADE_AREA, nematicref, nematic_prepareref, functional_mapnumericalgradient, nematic_integrand, NULL, NEMATIC_ARGS, SYMMETRY_NONE);

value Nematic_fieldgradient(vm *v, int nargs, value *args) {
    functional_mapinfo info;
    nematicref ref;
    value out=MORPHO_NIL;
    
    if (functional_validateargs(v, nargs, args, &info)) {
        if (nematic_prepareref(MORPHO_GETINSTANCE(MORPHO_SELF(args)), info.mesh, MESH_GRADE_AREA, info.sel, &ref)) {
            info.g=MESH_GRADE_AREA;
            info.integrand=nematic_integrand;
            info.ref=&ref;
            functional_mapnumericalfieldgradient(v, &info, &out);
        } else morpho_runtimeerror(v, GRADSQ_ARGS);
    }
    if (!MORPHO_ISNIL(out)) morpho_bindobjects(v, 1, &out);
    return out;
}

MORPHO_BEGINCLASS(Nematic)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, Nematic_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, Nematic_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, Nematic_total, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, Nematic_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_FIELDGRADIENT_METHOD, Nematic_fieldgradient, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * NematicElectric
 * ---------------------------------------------- */

typedef struct {
    objectfield *director;
    value field;
} nematicelectricref;

/** Prepares the nematicelectric reference */
bool nematicelectric_prepareref(objectinstance *self, objectmesh *mesh, grade g, objectselection *sel, nematicelectricref *ref) {
    bool success=false;
    ref->field=MORPHO_NIL;
    value fieldlist=MORPHO_NIL;
    
    if (objectinstance_getproperty(self, functional_fieldproperty, &fieldlist) &&
        MORPHO_ISLIST(fieldlist)) {
        objectlist *lst = MORPHO_GETLIST(fieldlist);
        value director = MORPHO_NIL;
        list_getelement(lst, 0, &director);
        list_getelement(lst, 1, &ref->field);
        
        if (MORPHO_ISFIELD(director)) ref->director=MORPHO_GETFIELD(director);
        
        if (MORPHO_ISFIELD(ref->field) || MORPHO_ISMATRIX(ref->field)) success=true;
    }
    
    return success;
}

/** Calculate the integral (n.E)^2 energy, where E is calculated from the electric potential */
bool nematicelectric_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    nematicelectricref *eref = ref;
    double size=0; // Length area or volume of the element
    
    if (!functional_elementsize(v, mesh, MESH_GRADE_AREA, id, nv, vid, &size)) return false;
    
    // Get nematic director components
    double *nn[nv]; // Field value lists
    unsigned int nentries=0;
    for (unsigned int i=0; i<nv; i++) {
        if (!field_getelementaslist(eref->director, MESH_GRADE_VERTEX, vid[i], 0, &nentries, &nn[i])) return false;
    }
    
    // The electric field ends up being constant over the element
    double ee[mesh->dim];
    if (MORPHO_ISFIELD(eref->field)) {
        if (!gradsq_evaluategradient(mesh, MORPHO_GETFIELD(eref->field), nv, vid, ee)) return false;
    }
    
    /* Calculate integrals of nx^2, ny^2, nz^2, nx*ny, ny*nz, and nz*nx over the element */
    double nnt[mesh->dim][nv]; // The transpose of nn
    for (unsigned int i=0; i<nv; i++)
        for (unsigned int j=0; j<mesh->dim; j++) nnt[j][i]=nn[i][j];
    
    /* Calculate integral (n.e)^2 using the above results */
    double total = ee[0]*ee[0]*nematic_bcint(nnt[0], nnt[0])+
                   ee[1]*ee[1]*nematic_bcint(nnt[1], nnt[1])+
                   ee[2]*ee[2]*nematic_bcint(nnt[2], nnt[2])+
                   2*ee[0]*ee[1]*nematic_bcint(nnt[0], nnt[1])+
                   2*ee[1]*ee[2]*nematic_bcint(nnt[1], nnt[2])+
                   2*ee[2]*ee[0]*nematic_bcint(nnt[2], nnt[0]);
    
    *out = size*total;
    
    return true;
}

/** Initialize a NematicElectric object */
value NematicElectric_init(vm *v, int nargs, value *args) {
    objectinstance *self = MORPHO_GETINSTANCE(MORPHO_SELF(args));
    
    if (nargs==2 && MORPHO_ISFIELD(MORPHO_GETARG(args, 0)) &&
        MORPHO_ISFIELD(MORPHO_GETARG(args, 1))) {
        objectlist *new = object_newlist(2, &MORPHO_GETARG(args, 0));
        if (new) {
            value lst = MORPHO_OBJECT(new);
            objectinstance_setproperty(self, functional_fieldproperty, lst);
            morpho_bindobjects(v, 1, &lst);
        }
    } else morpho_runtimeerror(v, NEMATICELECTRIC_ARGS);
    
    return MORPHO_NIL;
}

FUNCTIONAL_METHOD(NematicElectric, integrand, MESH_GRADE_AREA, nematicelectricref, nematicelectric_prepareref, functional_mapintegrand, nematicelectric_integrand, NULL, FUNCTIONAL_ARGS, SYMMETRY_NONE);

FUNCTIONAL_METHOD(NematicElectric, total, MESH_GRADE_AREA, nematicelectricref, nematicelectric_prepareref, functional_sumintegrand, nematicelectric_integrand, NULL, FUNCTIONAL_ARGS, SYMMETRY_NONE);

FUNCTIONAL_METHOD(NematicElectric, gradient, MESH_GRADE_AREA, nematicelectricref, nematicelectric_prepareref, functional_mapnumericalgradient, nematicelectric_integrand, NULL, FUNCTIONAL_ARGS, SYMMETRY_NONE);

value NematicElectric_fieldgradient(vm *v, int nargs, value *args) {
    functional_mapinfo info;
    nematicelectricref ref;
    value out=MORPHO_NIL;
    
    if (functional_validateargs(v, nargs, args, &info)) {
        if (nematicelectric_prepareref(MORPHO_GETINSTANCE(MORPHO_SELF(args)), info.mesh, MESH_GRADE_AREA, info.sel, &ref)) {
            info.g=MESH_GRADE_AREA;
            info.integrand=nematicelectric_integrand;
            info.ref=&ref;
            functional_mapnumericalfieldgradient(v, &info, &out);
        } else morpho_runtimeerror(v, GRADSQ_ARGS);
    }
    if (!MORPHO_ISNIL(out)) morpho_bindobjects(v, 1, &out);
    return out;
}

MORPHO_BEGINCLASS(NematicElectric)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, NematicElectric_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, NematicElectric_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, NematicElectric_total, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, NematicElectric_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_FIELDGRADIENT_METHOD, NematicElectric_fieldgradient, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * NormSq
 * ---------------------------------------------- */

/** Calculate the norm squared of a field quantity */
bool normsq_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    fieldref *eref = ref;
    unsigned int nentries;
    double *entries;
    
    if (field_getelementaslist(eref->field, MESH_GRADE_VERTEX, id, 0, &nentries, &entries)) {
        *out = functional_vecdot(nentries, entries, entries);
        return true;
    }
    
    return false;
}

FUNCTIONAL_METHOD(NormSq, integrand, MESH_GRADE_VERTEX, fieldref, gradsq_prepareref, functional_mapintegrand, normsq_integrand, NULL, GRADSQ_ARGS, SYMMETRY_NONE);

FUNCTIONAL_METHOD(NormSq, total, MESH_GRADE_VERTEX, fieldref, gradsq_prepareref, functional_sumintegrand, normsq_integrand, NULL, GRADSQ_ARGS, SYMMETRY_NONE);

FUNCTIONAL_METHOD(NormSq, gradient, MESH_GRADE_AREA, fieldref, gradsq_prepareref, functional_mapnumericalgradient, normsq_integrand, NULL, GRADSQ_ARGS, SYMMETRY_NONE);

value NormSq_fieldgradient(vm *v, int nargs, value *args) {
    functional_mapinfo info;
    fieldref ref;
    value out=MORPHO_NIL;
    
    if (functional_validateargs(v, nargs, args, &info)) {
        if (gradsq_prepareref(MORPHO_GETINSTANCE(MORPHO_SELF(args)), info.mesh, MESH_GRADE_VERTEX, info.sel, &ref)) {
            info.g=MESH_GRADE_VERTEX;
            info.ref=&ref;
            info.field=ref.field;
            info.integrand=normsq_integrand;
            functional_mapnumericalfieldgradient(v, &info, &out);
        } else morpho_runtimeerror(v, GRADSQ_ARGS);
    }
    if (!MORPHO_ISNIL(out)) morpho_bindobjects(v, 1, &out);
    return out;
}

MORPHO_BEGINCLASS(NormSq)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, GradSq_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, NormSq_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, NormSq_total, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, NormSq_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_FIELDGRADIENT_METHOD, NormSq_fieldgradient, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* **********************************************************************
 * Integrals
 * ********************************************************************** */

/* ----------------------------------------------
 * Integrand functions
 * ---------------------------------------------- */

value tangent;

static value functional_tangent(vm *v, int nargs, value *args) {
    return tangent;
}

/* ----------------------------------------------
 * LineIntegral
 * ---------------------------------------------- */

typedef struct {
    value integrand;
    int nfields;
    value *fields;
    vm *v;
} integralref;

/** Prepares an integral reference */
bool integral_prepareref(objectinstance *self, objectmesh *mesh, grade g, objectselection *sel, integralref *ref) {
    bool success=false;
    value func=MORPHO_NIL;
    value field=MORPHO_NIL;
    ref->v=NULL;
    ref->nfields=0;
    
    if (objectinstance_getproperty(self, scalarpotential_functionproperty, &func) &&
        MORPHO_ISCALLABLE(func)) {
        ref->integrand=func;
        success=true;
    }
    if (objectinstance_getproperty(self, functional_fieldproperty, &field) &&
        MORPHO_ISLIST(field)) {
        objectlist *list = MORPHO_GETLIST(field);
        ref->nfields=list->val.count;
        ref->fields=list->val.data;
    }
    return success;
}

bool integral_integrandfn(unsigned int dim, double *t, double *x, unsigned int nquantity, value *quantity, void *ref, double *fout) {
    integralref *iref = ref;
    objectmatrix posn = MORPHO_STATICMATRIX(x, dim, 1);
    value args[nquantity+1], out;
        
    args[0]=MORPHO_OBJECT(&posn);
    for (unsigned int i=0; i<nquantity; i++) args[i+1]=quantity[i];
    
    if (morpho_call(iref->v, iref->integrand, nquantity+1, args, &out)) {
        morpho_valuetofloat(out,fout);
        return true;
    }
    
    return false;
}

/** Integrate a function over a line */
bool lineintegral_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    integralref *iref = ref;
    double *x[2], size;
    bool success;
    
    if (!functional_elementsize(v, mesh, MESH_GRADE_LINE, id, nv, vid, &size)) return false;
    
    iref->v=v; 
    for (unsigned int i=0; i<nv; i++) {
        mesh_getvertexcoordinatesaslist(mesh, vid[i], &x[i]);
    }
    
    /* Set up tangent vector... (temporary code here) */
    double tangentdata[mesh->dim], tnorm=0.0;
    functional_vecsub(mesh->dim, x[1], x[0], tangentdata);
    tnorm=functional_vecnorm(mesh->dim, tangentdata);
    if (fabs(tnorm)>MORPHO_EPS) functional_vecscale(mesh->dim, 1.0/tnorm, tangentdata, tangentdata);
    objectmatrix mtangent = MORPHO_STATICMATRIX(tangentdata, mesh->dim, 1);
    tangent = MORPHO_OBJECT(&mtangent);
    
    value q0[iref->nfields+1], q1[iref->nfields+1];
    value *q[2] = { q0, q1 };
    for (unsigned int k=0; k<iref->nfields; k++) {
        for (unsigned int i=0; i<nv; i++) {
            field_getelement(MORPHO_GETFIELD(iref->fields[k]), MESH_GRADE_VERTEX, vid[i], 0, &q[i][k]);
        }
    }
    
    success=integrate_integrate(integral_integrandfn, mesh->dim, MESH_GRADE_LINE, x, iref->nfields, q, iref, out);
    if (success) *out *=size;
    
    return success;
}

FUNCTIONAL_METHOD(LineIntegral, integrand, MESH_GRADE_LINE, integralref, integral_prepareref, functional_mapintegrand, lineintegral_integrand, NULL, GRADSQ_ARGS, SYMMETRY_NONE);

FUNCTIONAL_METHOD(LineIntegral, total, MESH_GRADE_LINE, integralref, integral_prepareref, functional_sumintegrand, lineintegral_integrand, NULL, GRADSQ_ARGS, SYMMETRY_NONE);

FUNCTIONAL_METHOD(LineIntegral, gradient, MESH_GRADE_LINE, integralref, integral_prepareref, functional_mapnumericalgradient, lineintegral_integrand, NULL, GRADSQ_ARGS, SYMMETRY_NONE);

/** Initialize a LineIntegral object */
value LineIntegral_init(vm *v, int nargs, value *args) {
    objectinstance *self = MORPHO_GETINSTANCE(MORPHO_SELF(args));
    int nparams = -1, nfields = 0;
    
    if (nargs>0) {
        value f = MORPHO_GETARG(args, 0);
        
        if (morpho_countparameters(f, &nparams)) {
            objectinstance_setproperty(self, scalarpotential_functionproperty, MORPHO_GETARG(args, 0));
        } else {
            morpho_runtimeerror(v, LINEINTEGRAL_ARGS);
            return MORPHO_NIL;
        }
    }
    
    if (nparams!=nargs) {
        morpho_runtimeerror(v, LINEINTEGRAL_NFLDS);
        return MORPHO_NIL;
    }
    
    if (nargs>1) {
        /* Remaining arguments should be fields */
        objectlist *list = object_newlist(nargs-1, & MORPHO_GETARG(args, 1));
        if (!list) { morpho_runtimeerror(v, ERROR_ALLOCATIONFAILED); return MORPHO_NIL; }
        
        for (unsigned int i=1; i<nargs; i++) {
            if (!MORPHO_ISFIELD(MORPHO_GETARG(args, i))) {
                morpho_runtimeerror(v, LINEINTEGRAL_ARGS);
                object_free((object *) list);
                return MORPHO_NIL;
            }
        }
        
        value field = MORPHO_OBJECT(list);
        objectinstance_setproperty(self, functional_fieldproperty, field);
        morpho_bindobjects(v, 1, &field);
    }
    
    return MORPHO_NIL;
}

/** Field gradients for Line Integrals */
value LineIntegral_fieldgradient(vm *v, int nargs, value *args) {
    functional_mapinfo info;
    integralref ref;
    value out=MORPHO_NIL;
    
    if (functional_validateargs(v, nargs, args, &info)) {
        // Should check whether the field is known about here...
        if (integral_prepareref(MORPHO_GETINSTANCE(MORPHO_SELF(args)), info.mesh, MESH_GRADE_LINE, info.sel, &ref)) {
            info.g=MESH_GRADE_LINE;
            info.integrand=lineintegral_integrand;
            info.ref=&ref;
            functional_mapnumericalfieldgradient(v, &info, &out);
        } else morpho_runtimeerror(v, GRADSQ_ARGS);
    }
    if (!MORPHO_ISNIL(out)) morpho_bindobjects(v, 1, &out);
    return out;
}

MORPHO_BEGINCLASS(LineIntegral)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, LineIntegral_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, LineIntegral_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, LineIntegral_total, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, LineIntegral_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_FIELDGRADIENT_METHOD, LineIntegral_fieldgradient, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* ----------------------------------------------
 * AreaIntegral
 * ---------------------------------------------- */

/** Integrate a function over an area */
bool areaintegral_integrand(vm *v, objectmesh *mesh, elementid id, int nv, int *vid, void *ref, double *out) {
    integralref *iref = ref;
    double *x[3], size;
    bool success;
    
    if (!functional_elementsize(v, mesh, MESH_GRADE_AREA, id, nv, vid, &size)) return false;
    
    iref->v=v;
    for (unsigned int i=0; i<nv; i++) {
        mesh_getvertexcoordinatesaslist(mesh, vid[i], &x[i]);
    }
    
    value q0[iref->nfields+1], q1[iref->nfields+1], q2[iref->nfields+1];
    value *q[3] = { q0, q1, q2 };
    for (unsigned int k=0; k<iref->nfields; k++) {
        for (unsigned int i=0; i<nv; i++) {
            field_getelement(MORPHO_GETFIELD(iref->fields[k]), MESH_GRADE_VERTEX, vid[i], 0, &q[i][k]);
        }
    }
    
    success=integrate_integrate(integral_integrandfn, mesh->dim, MESH_GRADE_AREA, x, iref->nfields, q, iref, out);
    if (success) *out *=size;
    
    return success;
}

FUNCTIONAL_METHOD(AreaIntegral, integrand, MESH_GRADE_AREA, integralref, integral_prepareref, functional_mapintegrand, areaintegral_integrand, NULL, GRADSQ_ARGS, SYMMETRY_NONE);

FUNCTIONAL_METHOD(AreaIntegral, total, MESH_GRADE_AREA, integralref, integral_prepareref, functional_sumintegrand, areaintegral_integrand, NULL, GRADSQ_ARGS, SYMMETRY_NONE);

FUNCTIONAL_METHOD(AreaIntegral, gradient, MESH_GRADE_AREA, integralref, integral_prepareref, functional_mapnumericalgradient, areaintegral_integrand, NULL, GRADSQ_ARGS, SYMMETRY_NONE);

/** Field gradients for Area Integrals */
value AreaIntegral_fieldgradient(vm *v, int nargs, value *args) {
    functional_mapinfo info;
    integralref ref;
    value out=MORPHO_NIL;
    
    if (functional_validateargs(v, nargs, args, &info)) {
        // Should check whether the field is known about here...
        if (integral_prepareref(MORPHO_GETINSTANCE(MORPHO_SELF(args)), info.mesh, MESH_GRADE_AREA, info.sel, &ref)) {
            info.g=MESH_GRADE_AREA;
            info.integrand=areaintegral_integrand;
            info.ref=&ref;
            functional_mapnumericalfieldgradient(v, &info, &out);
        } else morpho_runtimeerror(v, GRADSQ_ARGS);
    }
    if (!MORPHO_ISNIL(out)) morpho_bindobjects(v, 1, &out);
    return out;
}

MORPHO_BEGINCLASS(AreaIntegral)
MORPHO_METHOD(MORPHO_INITIALIZER_METHOD, LineIntegral_init, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_INTEGRAND_METHOD, AreaIntegral_integrand, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_TOTAL_METHOD, AreaIntegral_total, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_GRADIENT_METHOD, AreaIntegral_gradient, BUILTIN_FLAGSEMPTY),
MORPHO_METHOD(FUNCTIONAL_FIELDGRADIENT_METHOD, AreaIntegral_fieldgradient, BUILTIN_FLAGSEMPTY)
MORPHO_ENDCLASS

/* **********************************************************************
 * Initialization
 * ********************************************************************** */

void functional_initialize(void) {
    functional_gradeproperty=builtin_internsymbolascstring(FUNCTIONAL_GRADE_PROPERTY);
    functional_fieldproperty=builtin_internsymbolascstring(FUNCTIONAL_FIELD_PROPERTY);
    scalarpotential_functionproperty=builtin_internsymbolascstring(SCALARPOTENTIAL_FUNCTION_PROPERTY);
    scalarpotential_gradfunctionproperty=builtin_internsymbolascstring(SCALARPOTENTIAL_GRADFUNCTION_PROPERTY);
    linearelasticity_referenceproperty=builtin_internsymbolascstring(LINEARELASTICITY_REFERENCE_PROPERTY);
    linearelasticity_poissonproperty=builtin_internsymbolascstring(LINEARELASTICITY_POISSON_PROPERTY);
    equielement_weightproperty=builtin_internsymbolascstring(EQUIELEMENT_WEIGHT_PROPERTY);
    nematic_ksplayproperty=builtin_internsymbolascstring(NEMATIC_KSPLAY_PROPERTY);
    nematic_ktwistproperty=builtin_internsymbolascstring(NEMATIC_KTWIST_PROPERTY);
    nematic_kbendproperty=builtin_internsymbolascstring(NEMATIC_KBEND_PROPERTY);
    nematic_pitchproperty=builtin_internsymbolascstring(NEMATIC_PITCH_PROPERTY);
    
    curvature_integrandonlyproperty=builtin_internsymbolascstring(CURVATURE_INTEGRANDONLY_PROPERTY);
    
    objectstring objclassname = MORPHO_STATICSTRING(OBJECT_CLASSNAME);
    value objclass = builtin_findclass(MORPHO_OBJECT(&objclassname));
    
    builtin_addclass(LENGTH_CLASSNAME, MORPHO_GETCLASSDEFINITION(Length), objclass);
    builtin_addclass(AREA_CLASSNAME, MORPHO_GETCLASSDEFINITION(Area), objclass);
    builtin_addclass(AREAENCLOSED_CLASSNAME, MORPHO_GETCLASSDEFINITION(AreaEnclosed), objclass);
    builtin_addclass(VOLUMEENCLOSED_CLASSNAME, MORPHO_GETCLASSDEFINITION(VolumeEnclosed), objclass);
    builtin_addclass(VOLUME_CLASSNAME, MORPHO_GETCLASSDEFINITION(Volume), objclass);
    builtin_addclass(SCALARPOTENTIAL_CLASSNAME, MORPHO_GETCLASSDEFINITION(ScalarPotential), objclass);
    builtin_addclass(LINEARELASTICITY_CLASSNAME, MORPHO_GETCLASSDEFINITION(LinearElasticity), objclass);
    builtin_addclass(EQUIELEMENT_CLASSNAME, MORPHO_GETCLASSDEFINITION(EquiElement), objclass);
    builtin_addclass(LINECURVATURESQ_CLASSNAME, MORPHO_GETCLASSDEFINITION(LineCurvatureSq), objclass);
    builtin_addclass(LINETORSIONSQ_CLASSNAME, MORPHO_GETCLASSDEFINITION(LineTorsionSq), objclass);
    builtin_addclass(MEANCURVATURESQ_CLASSNAME, MORPHO_GETCLASSDEFINITION(MeanCurvatureSq), objclass);
    builtin_addclass(GAUSSCURVATURE_CLASSNAME, MORPHO_GETCLASSDEFINITION(GaussCurvature), objclass);
    builtin_addclass(GRADSQ_CLASSNAME, MORPHO_GETCLASSDEFINITION(GradSq), objclass);
    builtin_addclass(NORMSQ_CLASSNAME, MORPHO_GETCLASSDEFINITION(NormSq), objclass);
    builtin_addclass(LINEINTEGRAL_CLASSNAME, MORPHO_GETCLASSDEFINITION(LineIntegral), objclass);
    builtin_addclass(AREAINTEGRAL_CLASSNAME, MORPHO_GETCLASSDEFINITION(AreaIntegral), objclass);
    builtin_addclass(NEMATIC_CLASSNAME, MORPHO_GETCLASSDEFINITION(Nematic), objclass);
    builtin_addclass(NEMATICELECTRIC_CLASSNAME, MORPHO_GETCLASSDEFINITION(NematicElectric), objclass);
    
    builtin_addfunction(TANGENT_FUNCTION, functional_tangent, BUILTIN_FLAGSEMPTY);
    
    morpho_defineerror(FUNC_INTEGRAND_MESH, ERROR_HALT, FUNC_INTEGRAND_MESH_MSG);
    morpho_defineerror(FUNC_ELNTFND, ERROR_HALT, FUNC_ELNTFND_MSG);
    morpho_defineerror(SCALARPOTENTIAL_FNCLLBL, ERROR_HALT, SCALARPOTENTIAL_FNCLLBL_MSG);
    morpho_defineerror(LINEARELASTICITY_REF, ERROR_HALT, LINEARELASTICITY_REF_MSG);
    morpho_defineerror(LINEARELASTICITY_PRP, ERROR_HALT, LINEARELASTICITY_PRP_MSG);
    morpho_defineerror(EQUIELEMENT_ARGS, ERROR_HALT, EQUIELEMENT_ARGS_MSG);
    morpho_defineerror(GRADSQ_ARGS, ERROR_HALT, GRADSQ_ARGS_MSG);
    morpho_defineerror(NEMATIC_ARGS, ERROR_HALT, NEMATIC_ARGS_MSG);
    morpho_defineerror(NEMATICELECTRIC_ARGS, ERROR_HALT, NEMATICELECTRIC_ARGS_MSG);
    morpho_defineerror(FUNCTIONAL_ARGS, ERROR_HALT, FUNCTIONAL_ARGS_MSG);
    morpho_defineerror(LINEINTEGRAL_ARGS, ERROR_HALT, LINEINTEGRAL_ARGS_MSG);
    morpho_defineerror(LINEINTEGRAL_NFLDS, ERROR_HALT, LINEINTEGRAL_NFLDS_MSG);
}
 
