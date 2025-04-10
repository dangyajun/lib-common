/***************************************************************************/
/*                                                                         */
/* Copyright 2022 INTERSEC SA                                              */
/*                                                                         */
/* Licensed under the Apache License, Version 2.0 (the "License");         */
/* you may not use this file except in compliance with the License.        */
/* You may obtain a copy of the License at                                 */
/*                                                                         */
/*     http://www.apache.org/licenses/LICENSE-2.0                          */
/*                                                                         */
/* Unless required by applicable law or agreed to in writing, software     */
/* distributed under the License is distributed on an "AS IS" BASIS,       */
/* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.*/
/* See the License for the specific language governing permissions and     */
/* limitations under the License.                                          */
/*                                                                         */
/***************************************************************************/

#ifndef IS_IOPY_CYTHON_EXPORT_FIX_H
#define IS_IOPY_CYTHON_EXPORT_FIX_H

#include <Python.h>
#include <lib-common/core.h>
#include <lib-common/iop.h>

/* Export public functions for the DSO. */
EXPORT const iop_struct_t *Iopy_struct_union_type_get_desc(PyObject *);
EXPORT bool Iopy_has_pytype_from_fullname(PyObject *);
EXPORT PyObject *Iopy_get_pytype_from_fullname_(PyObject *, lstr_t);
EXPORT bool Iopy_Struct_to_iop_ptr(mem_pool_t *, void **,
                                   const iop_struct_t *,
                                   PyObject *);
EXPORT bool Iopy_Union_to_iop_ptr(mem_pool_t *, void **, const iop_struct_t *,
                                  PyObject *);
EXPORT int Iopy_add_iop_dso(const iop_dso_t *, PyObject *);
EXPORT int Iopy_remove_iop_dso(const iop_dso_t *, PyObject *);
EXPORT PyObject *Iopy_from_iop_struct_or_union(PyObject *,
                                               const iop_struct_t *,
                                               const void *);
EXPORT PyObject *Iopy_make_plugin_iop_env(iop_env_t *iop_env);

#endif /* IS_IOPY_CYTHON_EXPORT_FIX_H */
