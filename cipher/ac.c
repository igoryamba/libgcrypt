/* ac.c - Alternative interface for asymmetric cryptography.
   Copyright (C) 2003 Free Software Foundation, Inc.
 
   This file is part of Libgcrypt.
  
   Libgcrypt is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser general Public License as
   published by the Free Software Foundation; either version 2.1 of
   the License, or (at your option) any later version.
  
   Libgcrypt is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.
  
   You should have received a copy of the GNU Lesser General Public
   License along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
   02111-1307, USA.  */

#include <config.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

#include "g10lib.h"
#include "cipher.h"
#include "ath.h"
#include "mpi.h"



/* These specifications are needed for key-pair generation; the caller
   is allowed to pass additional, algorithm-specific `specs' to
   gcry_ac_key_pair_generate.  This list is used for decoding the
   provided values according to the selected algorithm.  */
struct gcry_ac_key_generate_spec
{
  gcry_ac_id_t algorithm;	/* Algorithm for which this flag is
				   relevant.  */
  const char *name;		/* Name of this flag.  */
  size_t offset;		/* Offset in the cipher-specific spec
				   structure at which the MPI value
				   associated with this flag is to be
				   found.  */
} gcry_ac_key_generate_specs[] =
  {
    { GCRY_AC_RSA, "rsa-use-e", offsetof (gcry_ac_key_spec_rsa_t, e) },
    { 0 },
  };

/* Handle structure.  */
struct gcry_ac_handle
{
  gcry_ac_id_t algorithm;	/* Algorithm ID associated with this
				   handle.  */
  const char *algorithm_name;	/* Name of the algorithm.  */
  unsigned int flags;		/* Flags, not used yet.  */
  gcry_module_t module;	        /* Reference to the algorithm
				   module.  */
};

/* A named MPI value.  */
typedef struct gcry_ac_mpi
{
  const char *name;		/* Name ov MPI value. */
  gcry_mpi_t mpi;		/* MPI value.         */
} gcry_ac_mpi_t;

/* A data set, that is simply a list of named MPI values.  */
struct gcry_ac_data
{
  gcry_ac_mpi_t *data;		/* List of named values.      */
  unsigned int data_n;		/* Number of values in DATA.  */
};

/* A single key.  */
struct gcry_ac_key
{
  gcry_ac_data_t data;		/* Data in native ac structure.  */
  gcry_ac_key_type_t type;	/* Type of the key.              */
};

/* A key pair.  */
struct gcry_ac_key_pair
{
  gcry_ac_key_t public;		/* Public key.  */
  gcry_ac_key_t secret;		/* Secret key.  */
};



/* Call the function FUNCTION contained in the module MODULE and store
   the return code in ERR.  In case the module does not implement the
   specified function, an error code is returned directly.  */
#define AC_MODULE_CALL(err, module, function, ...)                               \
  {                                                                              \
    gcry_ac_##function##_t func = ((gcry_ac_spec_t *) (module)->spec)->function; \
    if (func)                                                                    \
      err = (*func) (__VA_ARGS__);                                               \
    else                                                                         \
      {                                                                          \
        log_bug ("ac: no implementation of `%s' for algorithm: %i\n",            \
                 #function, module->mod_id);                                     \
        err = GPG_ERR_PUBKEY_ALGO;                                               \
      }                                                                          \
  }



/* This is the list of the asymmetric algorithms included in the
   library.  */
static struct pubkey_table_entry
{
  gcry_ac_spec_t *algorithm;	/* Algorithm specification.  */
  gcry_ac_id_t algorithm_id;	/* Algorithm ID.             */
} algorithm_table[] =
  {
#if USE_RSA
    { &ac_spec_rsa, GCRY_AC_RSA },
#endif
#if USE_ELGAMAL
    { &ac_spec_elg, GCRY_AC_ELG },
#endif
#if USE_DSA
    { &ac_spec_dsa, GCRY_AC_DSA },
#endif
    { NULL },
  };

/* List of registered algorithm modules.  */
static gcry_module_t algorithms;

/* Lock protecting accesses to ALGORITHMS.  */
static ath_mutex_t algorithms_lock;

/* Locking macros.  */
#define ALGORITHMS_LOCK   ath_mutex_lock   (&algorithms_lock)
#define ALGORITHMS_UNLOCK ath_mutex_unlock (&algorithms_lock)

/* If non-zero, initialization of the ac subsystem is done.  */
static int ac_initialized;

/* Lock, protecting the initialization of the ac subsystem.  */
static ath_mutex_t ac_init_lock;

/* Locking macros.  */
#define AC_INIT_LOCK   ath_mutex_lock   (&ac_init_lock)
#define AC_INIT_UNLOCK ath_mutex_unlock (&ac_init_lock)

/* Convenient macro for initializing the ac subsystem.  */
#define AC_INIT               \
  do                          \
    {                         \
      AC_INIT_LOCK;           \
      if (! ac_initialized)   \
        {                     \
          ac_init ();         \
          ac_initialized = 1; \
        }                     \
      AC_INIT_UNLOCK;         \
    }                         \
  while (0)



/*
 * Progress callback support.
 */

static gcry_handler_progress_t progress_cb;
static void *progress_cb_data;

void
_gcry_ac_progress_register (gcry_handler_progress_t cb,
			    void *cb_data)
{
  progress_cb = cb;
  progress_cb_data = cb_data;
}

void
_gcry_ac_progress (const char *identifier, int c)
{
  if (progress_cb)
    progress_cb (progress_cb_data, identifier, c, 0, 0);
}

/*
 * Conversion between anonymous structures and data sets.
 */

/* Convert a data set into a newly created structure.  */
static gcry_err_code_t
anon_struct_from_data_set (gcry_ac_data_t data, size_t size,
			   unsigned int elems, gcry_ac_struct_spec_t *spec,
			   void **structure)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  void *structure_new = NULL;
  unsigned int i = 0;

  structure_new = gcry_malloc (size);
  if (! structure_new)
    err = GPG_ERR_INTERNAL;	/* FIXME!  */

  if (! err)
    for (i = 0; i < elems && (! err); i++)
      err = gcry_ac_data_get_name (data, spec[i].name,
				   (gcry_mpi_t *) (((char *) structure_new)
						   + spec[i].offset));

  if (! err)
    *structure = structure_new;
  else
    {
      if (structure_new)
	{
	  if (i)
	    for (i--; i >= 0; i--)
	      gcry_mpi_release (*((gcry_mpi_t *) (((char *) structure_new)
						  + spec[i].offset)));
	  gcry_free (structure_new);
	}
    }

  return err;
}

/* Create a new anonymous structure.  */
static gcry_err_code_t
anon_struct_create (size_t size, void **structure)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  void *structure_new = NULL;

  structure_new = gcry_malloc (size);
  if (! structure_new)
    err = GPG_ERR_INTERNAL;	/* FIXME!  */
  else
    memset (structure_new, 0, size);

  if (! err)
    *structure = structure_new;
  
  return err;
}

/* Convert a structure into a newly created data set.  */
static gcry_err_code_t
anon_struct_to_data_set (gcry_ac_data_t *data, unsigned int elems,
			 gcry_ac_struct_spec_t *spec,
			 void *structure)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_data_t data_new = NULL;
  unsigned int i = 0;

  err = gcry_ac_data_new (&data_new);

  if (! err)
    for (i = 0; (i < elems) && (! err); i++)
      err = gcry_ac_data_set (data_new, spec[i].name,
			      *((gcry_mpi_t *) (((char *) structure)
						+ spec[i].offset)));

  if (! err)
    *data = data_new;
  else
    {
      if (data_new)
	gcry_ac_data_destroy (data_new);
    }

  return err;
}

/* 
 * Module related functions.
 */

/* Internal callback function.  Used via _gcry_module_lookup.  */
static int
gcry_ac_lookup_func_name (void *spec, void *data)
{
  gcry_ac_spec_t *algorithm = (gcry_ac_spec_t *) spec;
  char *name = (char *) data;
  char **aliases = algorithm->aliases;
  int ret = stricmp (name, algorithm->name);

  while (ret && *aliases)
    ret = stricmp (name, *aliases++);

  return ! ret;
}

/* Internal function.  Lookup a pubkey entry by it's name.  */
static gcry_module_t 
gcry_ac_lookup_name (const char *name)
{
  gcry_module_t algorithm;

  algorithm = _gcry_module_lookup (algorithms, (void *) name,
				   gcry_ac_lookup_func_name);

  return algorithm;
}

/* Register a new algorithm module whose specification can be found in
   ALGORITHM.  On success, a new algorithm ID is stored in
   ALGORITHM_ID and a pointer representhing this module is stored in
   MODULE.  */
gcry_error_t
gcry_ac_register (gcry_ac_spec_t *algorithm,
		  unsigned int *algorithm_id,
		  gcry_module_t *module)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_module_t mod;

  ALGORITHMS_LOCK;
  err = _gcry_module_add (&algorithms, 0,
			  (void *) algorithm, &mod);
  ALGORITHMS_UNLOCK;

  if (! err)
    {
      *module = mod;
      *algorithm_id = mod->mod_id;
    }

  return err;
}

/* Unregister the algorithm identified by MODULE, which must have been
   registered with gcry_ac_register.  */
void
gcry_ac_unregister (gcry_module_t module)
{
  ALGORITHMS_LOCK;
  _gcry_module_release (module);
  ALGORITHMS_UNLOCK;
}

/* Internal function.  Register all the pubkeys included in
   PUBKEY_TABLE.  Returns zero on success or an error code.  */
static void
gcry_ac_register_algorithms (void)
{
  gcry_err_code_t err = 0;
  int i;

  ALGORITHMS_LOCK;

  for (i = 0; (! err) && algorithm_table[i].algorithm; i++)
    err = _gcry_module_add (&algorithms,
			    algorithm_table[i].algorithm_id,
			    (void *) algorithm_table[i].algorithm, NULL);

  ALGORITHMS_UNLOCK;

  if (err)
    BUG ();
}



/* 
 * Initialization.
 */

/* Initialize the ac subsystem.  */
static void
ac_init (void)
{
  gcry_ac_register_algorithms ();
}

/* Initialize the ac subsystem.  */
gcry_err_code_t
_gcry_ac_init (void)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  AC_INIT;

  return err;
}



/*
 * Primitive functions for the manipulation of `data sets'.
 */

/* Return in AC_MPI a pointer to the named MPI contained in DATA that
   is labelled with NAME or NULL in case there is no MPI with the that
   name.  */
static void
gcry_ac_data_search (gcry_ac_data_t data,
		     const char *name,
		     gcry_ac_mpi_t **ac_mpi)
{
  gcry_ac_mpi_t *ac_mpi_found = NULL;
  unsigned int i = 0;

  /* Search.  */
  for (i = 0; i < data->data_n; i++)
    if (! strcmp (name, data->data[i].name))
      ac_mpi_found = &data->data[i];

  *ac_mpi = ac_mpi_found;
}

/* Add MPI to DATA, with the label being NAME.  */
static gcry_err_code_t
gcry_ac_data_add (gcry_ac_data_t data,
		  const char *name, gcry_mpi_t mpi)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_mpi_t *ac_mpis = NULL;

  /* Allocate.  */
  ac_mpis = realloc (data->data,
		     sizeof (gcry_ac_mpi_t) * (data->data_n + 1));
  if (! ac_mpis)
    err = gpg_err_code_from_errno (errno);

  if (! err)
    {
      /* Fill. */
      if (ac_mpis != data->data)
	data->data = ac_mpis;
      data->data[data->data_n].name = name;
      data->data[data->data_n].mpi = mpi;
      data->data_n++;
    }

  return err;
}

/* Create a copy of the data set DATA and store it in DATA_CP.  */
static gcry_err_code_t
gcry_ac_data_copy_internal (gcry_ac_data_t *data_cp, gcry_ac_data_t data)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_data_t data_new = NULL;
  int i = 0;

  /* Allocate data set.  */
  data_new = gcry_malloc (sizeof (struct gcry_ac_data));
  if (! data_new)
    err = gpg_err_code_from_errno (errno);
  else
    data_new->data_n = data->data_n;

  if (! err)
    {
      /* Allocate space for named MPIs.  */
      data_new->data = gcry_malloc (sizeof (gcry_ac_mpi_t) * data->data_n);
      if (! data_new->data)
	err = gpg_err_code_from_errno (errno);
    }

  if (! err)
    {
      /* Copy named MPIs.  */
      
      for (i = 0; i < data_new->data_n && (! err); i++)
	{
	  data_new->data[i].name = NULL;
	  data_new->data[i].mpi = NULL;

	  /* Name.  */
	  data_new->data[i].name = strdup (data->data[i].name);
	  if (! data_new->data[i].name)
	    err = gpg_err_code_from_errno (errno);

	  if (! err)
	    {
	      /* MPI.  */
	      data_new->data[i].mpi = gcry_mpi_copy (data->data[i].mpi);
	      if (! data_new->data[i].mpi)
		err = gpg_err_code_from_errno (errno);
	    }
	}
    }

  if (! err)
    /* Copy out.  */
    *data_cp = data_new;
  else
    {
      /* Deallocate resources.  */
      if (data_new)
	{
	  if (data_new->data)
	    {
	      for (; i >= 0; i--)
		{
		  if (data_new->data[i].name)
		    free ((void *) data_new->data[i].name);
		  if (data_new->data[i].mpi)
		    gcry_mpi_release (data_new->data[i].mpi);
		}
	      gcry_free (data_new->data);
	    }
	  gcry_free (data_new);
	}
    }

  return err;
}



/* 
 * Functions for working with data sets.
 */

/* Creates a new, empty data set and stores it in DATA.  */
static gcry_err_code_t
_gcry_ac_data_new (gcry_ac_data_t *data)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_data_t data_new = NULL;

  data_new = gcry_malloc (sizeof (struct gcry_ac_data));
  if (! data_new)
    err = gpg_err_code_from_errno (errno);

  if (! err)
    {
      data_new->data = NULL;
      data_new->data_n = 0;
      *data = data_new;
    }

  return err;
}

/* Creates a new, empty data set and stores it in DATA.  */
gcry_error_t
gcry_ac_data_new (gcry_ac_data_t *data)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = _gcry_ac_data_new (data);

  return gcry_error (err);
}

/* Destroys the data set DATA.  */
void
_gcry_ac_data_destroy (gcry_ac_data_t data)
{
  int i;

  for (i = 0; i < data->data_n; i++)
    {
      gcry_free ((void *) data->data[i].name);
      gcry_mpi_release (data->data[i].mpi);
    }
  gcry_free (data->data);
  gcry_free (data);
}

/* Destroys the data set DATA.  */
void
gcry_ac_data_destroy (gcry_ac_data_t data)
{
  _gcry_ac_data_destroy (data);
}

/* Adds the value MPI to the data set DATA with the label NAME.  If
   there is already a value with that label, it is replaced, otherwise
   a new value is added. */
gcry_err_code_t
_gcry_ac_data_set (gcry_ac_data_t data,
		   const char *name, gcry_mpi_t mpi)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_mpi_t *ac_mpi;

  gcry_ac_data_search (data, name, &ac_mpi);
  if (ac_mpi)
    {
      /* An entry for NAME does already exist, replace it.  */
      if (ac_mpi->mpi != mpi)
	{
	  gcry_mpi_release (ac_mpi->mpi);
	  ac_mpi->mpi = mpi;
	}
    }
  else
    {
      /* Create a new entry.  */

      gcry_mpi_t mpi_cp = NULL;
      char *name_cp = NULL;

      name_cp = strdup (name);
      if (name_cp)
	mpi_cp = gcry_mpi_copy (mpi);
      if (! (name_cp && mpi_cp))
	err = gpg_err_code_from_errno (errno);

      if (! err)
	err = gcry_ac_data_add (data, name_cp, mpi_cp);

      if (err)
	{
	  if (name_cp)
	    gcry_free (name_cp);
	  if (mpi_cp)
	    gcry_mpi_release (mpi_cp);
	}
    }

  return err;
}

/* Adds the value MPI to the data set DATA with the label NAME.  If
   there is already a value with that label, it is replaced, otherwise
   a new value is added. */
gcry_error_t
gcry_ac_data_set (gcry_ac_data_t data,
		  const char *name, gcry_mpi_t mpi)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = _gcry_ac_data_set (data, name, mpi);

  return gcry_error (err);
}

/* Create a copy of the data set DATA and store it in DATA_CP.  */
gcry_error_t
gcry_ac_data_copy (gcry_ac_data_t *data_cp, gcry_ac_data_t data)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = gcry_ac_data_copy_internal (data_cp, data);

  return gcry_error (err);
}

/* Returns the number of named MPI values inside of the data set
   DATA.  */
unsigned int
gcry_ac_data_length (gcry_ac_data_t data)
{
  return data->data_n;
}

gcry_err_code_t
_gcry_ac_data_get_name (gcry_ac_data_t data, const char *name,
			gcry_mpi_t *mpi)
{
  gcry_err_code_t err = GPG_ERR_NO_DATA;
  gcry_mpi_t mpi_found = NULL;
  int i;
  
  for (i = 0; i < data->data_n && (! mpi_found); i++)
    if (! strcmp (data->data[i].name, name))
      {
	mpi_found = data->data[i].mpi;
	err = GPG_ERR_NO_ERROR;
      }

  if (! err)
    *mpi = mpi_found;

  return err;
}

/* Stores the value labelled with NAME found in the data set DATA in
   MPI.  The returned MPI value will be released in case
   gcry_ac_data_set is used to associate the label NAME with a
   different MPI value.  */
gcry_error_t
gcry_ac_data_get_name (gcry_ac_data_t data, const char *name,
		       gcry_mpi_t *mpi)
{
  gcry_err_code_t err = GPG_ERR_NO_DATA;

  err = _gcry_ac_data_get_name (data, name, mpi);

  return gcry_error (err);
}

gcry_err_code_t
_gcry_ac_arg_list_from_data (gcry_ac_data_t data, void ***arg_list)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  void **arg_list_new = NULL;
  unsigned int i = 0;

  if (data->data_n)
    {
      arg_list_new = gcry_malloc (sizeof (void *) * data->data_n);
      if (! arg_list_new)
	err = GPG_ERR_INTERNAL;	/* FIXME!  */

      if (! err)
	for (i = 0; i < data->data_n && (! err); i++)
	  arg_list_new[i] = &data->data[i].mpi;
    }

  if (! err)
    *arg_list = arg_list_new;
  else
    {
      if (arg_list_new)
	gcry_free (arg_list_new);
    }

  return err;
}

gcry_err_code_t
_gcry_ac_data_get_index (gcry_ac_data_t data, unsigned int index,
			 const char **name, gcry_mpi_t *mpi)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  if (index < data->data_n)
    {
      if (name)
	*name = data->data[index].name;
      if (mpi)
	*mpi = data->data[index].mpi;
    }
  else
    err = GPG_ERR_NO_DATA;

  return err;
}

/* Stores in NAME and MPI the named MPI value contained in the data
   set DATA with the index INDEX.  NAME or MPI may be NULL.  The
   returned MPI value will be released in case gcry_ac_data_set is
   used to associate the label NAME with a different MPI value.  */
gcry_error_t
gcry_ac_data_get_index (gcry_ac_data_t data, unsigned int index,
			const char **name, gcry_mpi_t *mpi)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = _gcry_ac_data_get_index (data, index, name, mpi);

  return gcry_error (err);
}

/* Destroys any values contained in the data set DATA.  */
void
gcry_ac_data_clear (gcry_ac_data_t data)
{
  gcry_free (data->data);
  data->data = NULL;
  data->data_n = 0;
}



/*
 * Handle management.
 */

/* Creates a new handle for the algorithm ALGORITHM and store it in
   HANDLE.  FLAGS is not used yet.  */
gcry_error_t
gcry_ac_open (gcry_ac_handle_t *handle,
	      gcry_ac_id_t algorithm, unsigned int flags)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_handle_t handle_new = NULL;
  gcry_module_t module = NULL;

  AC_INIT;

  ALGORITHMS_LOCK;

  module = _gcry_module_lookup_id (algorithms, algorithm);
  if ((! module) || (module->flags & FLAG_MODULE_DISABLED))
    err = GPG_ERR_PUBKEY_ALGO;

  if (! err)
    {
      /* Allocate.  */
      handle_new = gcry_malloc (sizeof (struct gcry_ac_handle));
      if (! handle_new)
	err = gpg_err_code_from_errno (errno);
    }

  if (! err)
    {
      /* Done.  */
      handle_new->algorithm = algorithm;
      handle_new->flags = flags;
      handle_new->module = module;
      *handle = handle_new;
    }
  else
    {
      /* Deallocate resources.  */
      if (module)
	_gcry_module_release (module);
    }

  ALGORITHMS_UNLOCK;

  return gcry_error (err);
}

/* Destroys the handle HANDLE.  */
void
gcry_ac_close (gcry_ac_handle_t handle)
{
  /* Release reference to pubkey module.  */
  ALGORITHMS_LOCK;
  _gcry_module_release (handle->module);
  ALGORITHMS_UNLOCK;
  gcry_free (handle);
}



/* 
 * Key management.
 */

/* Creates a new key of type TYPE, consisting of the MPI values
   contained in the data set DATA and stores it in KEY.  */
gcry_error_t
gcry_ac_key_init (gcry_ac_key_t *key,
		  gcry_ac_handle_t handle,
		  gcry_ac_key_type_t type,
		  gcry_ac_data_t data)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_data_t data_new = NULL;
  gcry_ac_key_t key_new = NULL;

  /* Allocate.  */
  key_new = gcry_malloc (sizeof (struct gcry_ac_key));
  if (! key_new)
    err = gpg_err_code_from_errno (errno);

  if (! err)
    /* Copy data set.  */
    err = gcry_ac_data_copy_internal (&data_new, data);

  if (! err)
    {
      /* Done.  */
      key_new->data = data_new;
      key_new->type = type;
      *key = key_new;
    }
  else
    {
      /* Deallocate resources.  */
      if (key_new)
	gcry_free (key_new);
    }

  return gcry_error (err);
}

/* Generates a new key pair via the handle HANDLE of NBITS bits and
   stores it in KEY_PAIR.  In case non-standard settings are wanted, a
   pointer to a structure of type gcry_ac_key_spec_<algorithm>_t,
   matching the selected algorithm, can be given as KEY_SPEC.  If
   MISC_DATA is non-zero, return an algorithm-dependent list of MPI
   values that were created during the generation process.  */
static gcry_err_code_t
key_pair_generate (gcry_ac_handle_t handle,
		   unsigned int nbits,
		   void *key_spec,
		   gcry_ac_key_pair_t *key_pair,
		   gcry_mpi_t **misc_data)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_key_pair_t key_pair_new = NULL;
  gcry_ac_data_t key_data_secret = NULL;
  gcry_ac_data_t key_data_public = NULL;
  gcry_mpi_t *misc_data_new = NULL;
  void *key_secret = NULL;
  void *key_public = NULL;
  unsigned int i = 0;

  err = anon_struct_create ((((gcry_ac_spec_t *)
			      handle->module->spec)->size_key_public),
			    &key_public);
  if (! err)
    err = anon_struct_create ((((gcry_ac_spec_t *)
				handle->module->spec)->size_key_secret),
			      &key_secret);

  /* Generate keys.  */
  if (! err)
    AC_MODULE_CALL (err, handle->module, generate,
		    nbits, key_spec, key_secret, key_public, &misc_data_new);

  /* Convert anonymous structures into data sets.  */
  if (! err)
    err = anon_struct_to_data_set (&key_data_secret,
				   (((gcry_ac_spec_t *)
				     handle->module->spec)->elems_key_secret),
				   (((gcry_ac_spec_t *)
				     handle->module->spec)->spec_key_secret),
				   key_secret);
  if (! err)
    err = anon_struct_to_data_set (&key_data_public,
				   (((gcry_ac_spec_t *)
				     handle->module->spec)->elems_key_public),
				   (((gcry_ac_spec_t *)
				     handle->module->spec)->spec_key_public),
				   key_public);

  if (! err)
    {
      /* Allocate key pair.  */
      key_pair_new = gcry_malloc (sizeof (struct gcry_ac_key_pair));
      if (! key_pair_new)
	err = gpg_err_code_from_errno (errno);
    }

  if (! err)
    {
      /* Allocate keys.  */
      key_pair_new->secret = gcry_malloc (sizeof (struct gcry_ac_key));
      key_pair_new->public = gcry_malloc (sizeof (struct gcry_ac_key));
      
      if (! (key_pair_new->secret || key_pair_new->public))
	err = gpg_err_code_from_errno (errno);
      else
	{
	  key_pair_new->secret->type = GCRY_AC_KEY_SECRET;
	  key_pair_new->public->type = GCRY_AC_KEY_PUBLIC;
	  key_pair_new->secret->data = key_data_secret;
	  key_pair_new->public->data = key_data_public;
	}
    }

  /* Done.  */

  if (key_secret)
    gcry_free (key_secret);
  if (key_public)
    gcry_free (key_public);

  if (! err)
    {
      *key_pair = key_pair_new;
      if (misc_data)
	*misc_data = misc_data_new;
      else if (misc_data_new)
	{
	  for (i = 0; misc_data_new[i]; i++)
	    gcry_mpi_release (misc_data_new[i]);
	  gcry_free (misc_data_new);
	}
    }
  else
    {
      /* Deallocate resources.  */

      if (key_data_public)
	gcry_ac_data_destroy (key_data_public);
      if (key_data_secret)
	gcry_ac_data_destroy (key_data_secret);
      if (misc_data_new)
	gcry_free (misc_data_new);

      if (key_pair_new)
	{
	  if (key_pair_new->secret)
	    {
	      key_pair_new->secret->data = NULL;
	      gcry_ac_key_destroy (key_pair_new->secret);
	    }
	  if (key_pair_new->public)
	    {
	      key_pair_new->public->data = NULL;
	      gcry_ac_key_destroy (key_pair_new->public);
	    }
	  gcry_free (key_pair_new);
	}
    }

  return err;
}

/* Generates a new key pair via the handle HANDLE of NBITS bits and
   stores it in KEY_PAIR.  In case non-standard settings are wanted, a
   pointer to a structure of type gcry_ac_key_spec_<algorithm>_t,
   matching the selected algorithm, can be given as KEY_SPEC.  */
gcry_error_t
gcry_ac_key_pair_generate (gcry_ac_handle_t handle,
			   unsigned int nbits,
			   void *key_spec,
			   gcry_ac_key_pair_t *key_pair)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = key_pair_generate (handle, nbits, key_spec, key_pair, NULL);

  return gcry_error (err);
}

/* Generates a new key pair via the handle HANDLE of NBITS bits and
   stores it in KEY_PAIR.  In case non-standard settings are wanted, a
   pointer to a structure of type gcry_ac_key_spec_<algorithm>_t,
   matching the selected algorithm, can be given as KEY_SPEC.  If
   MISC_DATA is non-zero, return an algorithm-dependent list of MPI
   values that were created during the generation process.  */
gcry_err_code_t
gcry_ac_key_pair_generate_ext (gcry_ac_handle_t handle,
			       unsigned int nbits,
			       void *key_spec,
			       gcry_ac_key_pair_t *key_pair,
			       gcry_mpi_t **misc_data)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = key_pair_generate (handle, nbits, key_spec, key_pair, misc_data);

  return gcry_error (err);
}  

/* Returns the key of type WHICH out of the key pair KEY_PAIR.  */
gcry_ac_key_t
gcry_ac_key_pair_extract (gcry_ac_key_pair_t key_pair,
			  gcry_ac_key_type_t witch)
{
  gcry_ac_key_t key = NULL;

  switch (witch)
    {
    case GCRY_AC_KEY_SECRET:
      key = key_pair->secret;
      break;

    case GCRY_AC_KEY_PUBLIC:
      key = key_pair->public;
      break;
    }

  return key;
}

/* Destroys the key KEY.  */
void
gcry_ac_key_destroy (gcry_ac_key_t key)
{
  unsigned int i;
  
  if (key->data)
    {
      for (i = 0; i < key->data->data_n; i++)
	if (key->data->data[i].mpi != NULL)
	  gcry_mpi_release (key->data->data[i].mpi);
      gcry_free (key->data);
    }
  gcry_free (key);
}

/* Destroys the key pair KEY_PAIR.  */
void
gcry_ac_key_pair_destroy (gcry_ac_key_pair_t key_pair)
{
  gcry_ac_key_destroy (key_pair->secret);
  gcry_ac_key_destroy (key_pair->public);
  gcry_free (key_pair);
}

/* Returns the data set contained in the key KEY.  */
gcry_ac_data_t
gcry_ac_key_data_get (gcry_ac_key_t key)
{
  return key->data;
}

/* Verifies that the key KEY is sane.  */
gcry_error_t
gcry_ac_key_test (gcry_ac_handle_t handle,
		  gcry_ac_key_t key)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  void *key_structure = NULL;

  err = anon_struct_from_data_set (key->data,
				   (((gcry_ac_spec_t *)
				     handle->module->spec)->size_key_secret),
				   (((gcry_ac_spec_t *)
				     handle->module->spec)->elems_key_secret),
				   (((gcry_ac_spec_t *)
				     handle->module->spec)->spec_key_secret),
				   &key_structure);
  if (! err)
    AC_MODULE_CALL (err, handle->module, key_secret_check, key_structure);

  if (key_structure)
    gcry_free (key_structure);

  return gcry_error (err);
}

/* Stores the number of bits of the key KEY in NBITS.  */
gcry_err_code_t
_gcry_ac_key_get_nbits (gcry_ac_handle_t handle,
			gcry_ac_key_t key, unsigned int *nbits)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  void *key_struct_public = NULL;
  void *key_struct_secret = NULL;
  gcry_ac_struct_spec_t *spec = NULL;
  unsigned int nbits_new = 0;
  unsigned int elems = 0;
  size_t size = 0;

  switch (key->type)
    {
    case GCRY_AC_KEY_PUBLIC:
      spec = (((gcry_ac_spec_t *) handle->module->spec)->spec_key_public);
      size = (((gcry_ac_spec_t *) handle->module->spec)->size_key_public);
      elems = (((gcry_ac_spec_t *) handle->module->spec)->elems_key_public);
      break;

    case GCRY_AC_KEY_SECRET:
      spec = (((gcry_ac_spec_t *) handle->module->spec)->spec_key_secret);
      size = (((gcry_ac_spec_t *) handle->module->spec)->size_key_secret);
      elems = (((gcry_ac_spec_t *) handle->module->spec)->elems_key_secret);
      break;

    default:
      err = GPG_ERR_INTERNAL;	/* FIXME?  */
    }

  if (! err)
    {
      if (key->type == GCRY_AC_KEY_PUBLIC)
	err = anon_struct_from_data_set (key->data, size, elems, spec,
					 &key_struct_public);
      else
	err = anon_struct_from_data_set (key->data, size, elems, spec,
					 &key_struct_secret);
    }
  
  if (! err)
    AC_MODULE_CALL (err, handle->module, get_nbits,
		    key_struct_public, key_struct_secret, &nbits_new);

  if (key_struct_public)
    gcry_free (key_struct_public);
  if (key_struct_secret)
    gcry_free (key_struct_secret);
  
  if (! err)
    *nbits = nbits_new;

  return err;
}

gcry_error_t
gcry_ac_key_get_nbits (gcry_ac_handle_t handle,
		       gcry_ac_key_t key, unsigned int *nbits)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = _gcry_ac_key_get_nbits (handle, key, nbits);

  return gcry_error (err);
}
  
gcry_err_code_t
_gcry_ac_key_get_grip_std (unsigned char *key_grip, unsigned int flags, ...)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  unsigned char *mpi_buffer = NULL;
  size_t mpi_buffer_size = 0;
  gcry_md_hd_t md_handle = NULL;
  const char *name = NULL;
  gcry_mpi_t mpi = NULL;
  va_list ap;

  /* Create handle for hashing.  */
  err = gcry_err_code (gcry_md_open (&md_handle, GCRY_MD_SHA1, 0));

  if (! err)
    {
      va_start (ap, flags);
      
      /* Iterate over provided data and write it to message digest
	 handle.  */
      do
	{
	  name = va_arg (ap, const char *);
	  if (name)
	    {
	      mpi = va_arg (ap, gcry_mpi_t);

	      err = gcry_mpi_aprint (GCRYMPI_FMT_USG,
				     &mpi_buffer, &mpi_buffer_size, mpi);
	      if (! err)
		{
		  if (flags & GCRY_AC_KEY_GRIP_FLAG_SEXP)
		    {
		      /* FIXME, this is not so nice.  */
		      char buf[30];
		      
		      sprintf (buf, "(1:%c%u:", *name, (unsigned int) mpi_buffer_size);
		      gcry_md_write (md_handle, buf, strlen (buf));
		    }
		  gcry_md_write (md_handle, mpi_buffer, mpi_buffer_size);
		  if (flags & GCRY_AC_KEY_GRIP_FLAG_SEXP)
		    gcry_md_write (md_handle, ")", 1);
		}
	    }
	}
      while (name);
    }

  if (! err)
    memcpy (key_grip, gcry_md_read (md_handle, GCRY_MD_SHA1), 20);

  if (md_handle)
    gcry_md_close (md_handle);

  return err;
}

/* Writes the 20 byte long key grip of the key KEY to KEY_GRIP.  */
gcry_error_t
gcry_ac_key_get_grip (gcry_ac_handle_t handle,
		      gcry_ac_key_t key, unsigned char *key_grip)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  void *key_structure = NULL;

  if (key->type != GCRY_AC_KEY_PUBLIC)
    err = GPG_ERR_INTERNAL;	/* FIXME!!  */
  
  if (! err)
    err = anon_struct_from_data_set (key->data,
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->size_key_public),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->elems_key_public),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->spec_key_public),
				     &key_structure);
  if (! err)
    AC_MODULE_CALL (err, handle->module, get_grip, key_structure, key_grip);

  if (key_structure)
    gcry_free (key_structure);

  return gcry_error (err);
}



/* 
 * Functions performing cryptographic operations.
 */

gcry_err_code_t
_gcry_ac_data_encrypt (gcry_ac_handle_t handle,
		       unsigned int flags,
		       gcry_ac_key_t key,
		       gcry_mpi_t data_plain,
		       gcry_ac_data_t *data_encrypted)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_data_t data_encrypted_new = NULL;
  void *data_encrypted_struct = NULL;
  void *key_struct = NULL;

  if (key->type != GCRY_AC_KEY_PUBLIC)
    err = GPG_ERR_WRONG_KEY_USAGE;

  if (! err)
    /* Convert key.  */
    err = anon_struct_from_data_set (key->data,
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->size_key_public),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->elems_key_public),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->spec_key_public),
				     &key_struct);

  if (! err)
    /* Create anonymous struct.  */
    err = anon_struct_create ((((gcry_ac_spec_t *)
				handle->module->spec)->size_data_encrypted),
			      &data_encrypted_struct);

  if (! err)
    /* Encrypt.  */
    AC_MODULE_CALL (err, handle->module, encrypt,
		    data_plain, key_struct, data_encrypted_struct, flags);

  /* Convert encrypted data into data set.  */
  if (! err)
    err = anon_struct_to_data_set (&data_encrypted_new,
				   ((gcry_ac_spec_t *)
				    handle->module->spec)->elems_data_encrypted,
				   ((gcry_ac_spec_t *)
				    handle->module->spec)->spec_data_encrypted,
				   data_encrypted_struct);

  /* Deallocate resources.  */

  if (key_struct)
    gcry_free (key_struct);
  if (data_encrypted_struct)
    gcry_free (data_encrypted_struct);

  if (! err)
    *data_encrypted = data_encrypted_new;
  else
    {
      if (data_encrypted_new)
	gcry_ac_data_destroy (data_encrypted_new);
    }

  return err;
}

/* Encrypts the plain text MPI value DATA_PLAIN with the key public
   KEY under the control of the flags FLAGS and stores the resulting
   data set into DATA_ENCRYPTED.  */
gcry_error_t
gcry_ac_data_encrypt (gcry_ac_handle_t handle,
		      unsigned int flags,
		      gcry_ac_key_t key,
		      gcry_mpi_t data_plain,
		      gcry_ac_data_t *data_encrypted)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = _gcry_ac_data_encrypt (handle, flags, key, data_plain, data_encrypted);

  return gcry_error (err);
}

gcry_err_code_t
_gcry_ac_data_decrypt (gcry_ac_handle_t handle,
		       unsigned int flags,
		       gcry_ac_key_t key,
		       gcry_mpi_t *data_decrypted,
		       gcry_ac_data_t data_encrypted)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_mpi_t data_decrypted_new = NULL;
  void *data_encrypted_struct = NULL;
  void *key_struct = NULL;

  if (key->type != GCRY_AC_KEY_SECRET)
    err = GPG_ERR_WRONG_KEY_USAGE;

  if (! err)
    /* Convert key.  */
    err = anon_struct_from_data_set (key->data,
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->size_key_secret),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->elems_key_secret),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->spec_key_secret),
				     &key_struct);

  if (! err)
    /* Convert data.  */
    err = anon_struct_from_data_set (data_encrypted,
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->size_data_encrypted),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->elems_data_encrypted),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->spec_data_encrypted),
				     &data_encrypted_struct);

  if (! err)
    /* Decrypt.  */
    AC_MODULE_CALL (err, handle->module, decrypt,
		    data_encrypted_struct, key_struct, &data_decrypted_new,
		    flags);

  /* Deallocate resources.  */
  if (key_struct)
    gcry_free (key_struct);
  if (data_encrypted_struct)
    gcry_free (data_encrypted_struct);

  /* Done.  */

  if (! err)
    *data_decrypted = data_decrypted_new;
  else
    {
      if (data_decrypted_new)
	gcry_mpi_release (data_decrypted_new);
    }

  return err;
}

/* Decrypts the encrypted data contained in the data set
   DATA_ENCRYPTED with the secret key KEY under the control of the
   flags FLAGS and stores the resulting plain text MPI value in
   DATA_PLAIN.  */
gcry_err_code_t
gcry_ac_data_decrypt (gcry_ac_handle_t handle,
		      unsigned int flags,
		      gcry_ac_key_t key,
		      gcry_mpi_t *data_decrypted,
		      gcry_ac_data_t data_encrypted)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = _gcry_ac_data_decrypt (handle, flags, key, data_decrypted, data_encrypted);
  
  return gcry_error (err);
}

/* Signs the data contained in DATA with the secret key KEY and stores
   the resulting signature data set in DATA_SIGNATURE.  */
gcry_err_code_t
_gcry_ac_data_sign (gcry_ac_handle_t handle,
		    gcry_ac_key_t key,
		    gcry_mpi_t data,
		    gcry_ac_data_t *data_signed)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_data_t data_signed_new = NULL;
  void *data_signed_struct = NULL;
  void *key_struct = NULL;

  if (key->type != GCRY_AC_KEY_SECRET)
    err = GPG_ERR_WRONG_KEY_USAGE;

  if (! err)
    /* Convert key.  */
    err = anon_struct_from_data_set (key->data,
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->size_key_secret),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->elems_key_secret),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->spec_key_secret),
				     &key_struct);

  if (! err)
    /* Create anonymous struct.  */
    err = anon_struct_create ((((gcry_ac_spec_t *)
				handle->module->spec)->size_data_signed),
			      &data_signed_struct);

  if (! err)
    /* Sign.  */
    AC_MODULE_CALL (err, handle->module, sign,
		    data, key_struct, data_signed_struct);

  /* Convert signed data into data set.  */
  if (! err)
    err = anon_struct_to_data_set (&data_signed_new,
				   ((gcry_ac_spec_t *)
				    handle->module->spec)->elems_data_signed,
				   ((gcry_ac_spec_t *)
				    handle->module->spec)->spec_data_signed,
				   data_signed_struct);

  /* Deallocate resources.  */
  if (key_struct)
    gcry_free (key_struct);
  if (data_signed_struct)
    gcry_free (data_signed_struct);
  
  if (! err)
    *data_signed = data_signed_new;
  else
    {
      if (data_signed_new)
	gcry_ac_data_destroy (data_signed_new);
    }

  return err;
}

/* Signs the data contained in DATA with the secret key KEY and stores
   the resulting signature data set in DATA_SIGNATURE.  */
gcry_error_t
gcry_ac_data_sign (gcry_ac_handle_t handle,
		   gcry_ac_key_t key,
		   gcry_mpi_t data,
		   gcry_ac_data_t *data_signed)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = _gcry_ac_data_sign (handle, key, data, data_signed);

  return gcry_error (err);
}

/* Verifies that the signature contained in the data set
   DATA_SIGNATURE is indeed the result of signing the data contained
   in DATA with the secret key belonging to the public key KEY.  */
gcry_err_code_t
_gcry_ac_data_verify (gcry_ac_handle_t handle,
		      gcry_ac_key_t key,
		      gcry_mpi_t data,
		      gcry_ac_data_t data_signed)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  void *data_signed_struct = NULL;
  void *key_struct = NULL;

  if (key->type != GCRY_AC_KEY_PUBLIC)
    err = GPG_ERR_WRONG_KEY_USAGE;

  if (! err)
    /* Convert key.  */
    err = anon_struct_from_data_set (key->data,
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->size_key_public),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->elems_key_public),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->spec_key_public),
				     &key_struct);
  
  if (! err)
    /* Convert signed data.  */
    err = anon_struct_from_data_set (data_signed,
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->size_data_signed),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->elems_data_signed),
				     (((gcry_ac_spec_t *)
				       handle->module->spec)->spec_data_signed),
				     &data_signed_struct);

  if (! err)
    /* Verify signature.  */
    AC_MODULE_CALL (err, handle->module, verify,
		    data, key_struct, data_signed_struct);

  /* Deallocate resources.  */
  if (key_struct)
    gcry_free (key_struct);
  if (data_signed_struct)
    gcry_free (data_signed_struct);

  return err;
}

/* Verifies that the signature contained in the data set
   DATA_SIGNATURE is indeed the result of signing the data contained
   in DATA with the secret key belonging to the public key KEY.  */
gcry_error_t
gcry_ac_data_verify (gcry_ac_handle_t handle,
		     gcry_ac_key_t key,
		     gcry_mpi_t data,
		     gcry_ac_data_t data_signed)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = _gcry_ac_data_verify (handle, key, data, data_signed);

  return gcry_error (err);
}



/* 
 * General functions.
 */

/* Stores the textual representation of the algorithm whose id is
   given in ALGORITHM in NAME.  */
gcry_error_t
gcry_ac_id_to_name (gcry_ac_id_t algorithm_id, const char **algorithm_name)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_module_t module = NULL;
  const char *name = NULL;
  
  AC_INIT;

  ALGORITHMS_LOCK;
  module = _gcry_module_lookup_id (algorithms, algorithm_id);
  if (module)
    {
      name = strdup (((gcry_ac_spec_t *) module->spec)->name);
      if (! name)
	err = gpg_err_code_from_errno (ENOMEM);
      _gcry_module_release (module);
    }
  else
    err = GPG_ERR_PUBKEY_ALGO;
  ALGORITHMS_UNLOCK;

  if (! err)
    *algorithm_name = name;

  return gcry_error (err);
}

/* Stores the numeric ID of the algorithm whose textual representation
   is contained in NAME in ALGORITHM.  */
gcry_error_t
gcry_ac_name_to_id (const char *name, gcry_ac_id_t *algorithm_id)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_module_t module = NULL;
  int mod_id = 0;

  AC_INIT;

  ALGORITHMS_LOCK;
  module = gcry_ac_lookup_name (name);
  if (module)
    {
      mod_id = module->mod_id;
      _gcry_module_release (module);
    }
  else
    err = GPG_ERR_PUBKEY_ALGO;
  ALGORITHMS_UNLOCK;

  if (! err)
    *algorithm_id = mod_id;

  return gcry_error (err);
}

/* Get a list consisting of the IDs of the loaded algorithm modules.
   If LIST is zero, write the number of loaded pubkey modules to
   LIST_LENGTH and return.  If LIST is non-zero, the first
   *LIST_LENGTH algorithm IDs are stored in LIST, which must be of
   according size.  In case there are less pubkey modules than
   *LIST_LENGTH, *LIST_LENGTH is updated to the correct number.  */
gcry_error_t
gcry_ac_list (int *list, int *list_length)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  ALGORITHMS_LOCK;
  err = _gcry_module_list (algorithms, list, list_length);
  ALGORITHMS_UNLOCK;

  return err;
}

gcry_error_t
_gcry_ac_algorithm_enable (gcry_ac_handle_t handle)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  ALGORITHMS_LOCK;
  handle->module->flags &= ~FLAG_MODULE_DISABLED;
  ALGORITHMS_UNLOCK;

  return gpg_error (err);
}

gcry_error_t
_gcry_ac_algorithm_disable (gcry_ac_handle_t handle)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  ALGORITHMS_LOCK;
  handle->module->flags |= FLAG_MODULE_DISABLED;
  ALGORITHMS_UNLOCK;

  return gpg_error (err);
}

static void
elements_amount_get (gcry_ac_struct_spec_t *spec, unsigned int *amount)
{
  unsigned int i = 0;

  for (i = 1; spec[i].name; i++);
  *amount = i;
}
 
void
_gcry_ac_elements_amount_get (gcry_ac_handle_t handle,
			      unsigned int *elements_key_secret,
			      unsigned int *elements_key_public,
			      unsigned int *elements_data_encrypted,
			      unsigned int *elements_data_signed)
{
  if (elements_key_secret)
    elements_amount_get (((gcry_ac_spec_t *)
			  handle->module->spec)->spec_key_secret,
			 elements_key_secret);
  if (elements_key_public)
    elements_amount_get (((gcry_ac_spec_t *)
			  handle->module->spec)->spec_key_public,
			 elements_key_public);
  if (elements_data_encrypted)
    elements_amount_get (((gcry_ac_spec_t *)
			  handle->module->spec)->spec_data_encrypted,
			 elements_data_encrypted);
  if (elements_data_signed)
    elements_amount_get (((gcry_ac_spec_t *)
			  handle->module->spec)->spec_data_signed,
			 elements_data_signed);
}

void
_gcry_ac_info_get (gcry_ac_handle_t handle,
		   gcry_ac_id_t *algorithm_id, unsigned int *algorithm_use_flags)
{
  if (algorithm_id)
    *algorithm_id = handle->algorithm;
  if (algorithm_use_flags)
    *algorithm_use_flags = (0
			    | (((gcry_ac_spec_t *)
				handle->module->spec)->sign
			       ? GCRY_PK_USAGE_SIGN : 0)
			    | (((gcry_ac_spec_t *)
				handle->module->spec)->encrypt
			       ? GCRY_PK_USAGE_ENCR : 0));
}

/*
 * Implementation of encoding methods (em).
 */

/* Type for functions that encode or decode (hence the name) a
   message.  */
typedef gcry_err_code_t (*gcry_ac_em_dencode_t) (unsigned int flags, void *options,
						 unsigned char *in, size_t in_n,
						 unsigned char **out, size_t *out_n);

/* Fill the buffer BUFFER which is BUFFER_N bytes long with non-zero
   random bytes of random level LEVEL.  */
static void
em_randomize_nonzero (unsigned char *buffer, size_t buffer_n,
		      gcry_random_level_t level)
{
  unsigned char *buffer_rand = NULL;
  unsigned int buffer_rand_n = 0;
  unsigned int i = 0, j = 0;
  unsigned int zeros = 0;

  for (i = 0; i < buffer_n; i++)
    buffer[i] = 0;
  
  do
    {
      /* Count zeros.  */
      for (i = zeros = 0; i < buffer_n; i++)
	if (! buffer[i])
	  zeros++;

      if (zeros)
	{
	  /* Get random bytes.  */
	  buffer_rand_n = zeros + (zeros / 128);
	  buffer_rand = gcry_random_bytes_secure (buffer_rand_n, level);

	  /* Substitute zeros with non-zero random bytes.  */
	  for (i = j = 0; zeros && (i < buffer_n) && (j < buffer_rand_n); i++)
	    if (! buffer[i])
	      {
		while ((j < buffer_rand_n) && (! buffer_rand[j]))
		  j++;
		if (j < buffer_rand_n)
		  {
		    buffer[i] = buffer_rand[j++];
		    zeros--;
		  }
		else
		  break;
	      }
	  gcry_free (buffer_rand);
	}
    }
  while (zeros);
}

/* Encode a message according to the Encoding Method for Signatures
   with Appendix `PKCS-V1_5' (EMSA-PKCS-V1_5).  */
static gcry_err_code_t
emsa_pkcs_v1_5_encode (unsigned int flags, void *opts,
		       unsigned char *m, size_t m_n,
		       unsigned char **em, size_t *em_n)
{
  gcry_ac_emsa_pkcs_v1_5_t *options = (gcry_ac_emsa_pkcs_v1_5_t *) opts;
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_md_hd_t handle = NULL;
  unsigned char *t = NULL;
  size_t t_n = 0;
  unsigned char *h = NULL;
  size_t h_n = 0;
  unsigned char *ps = NULL;
  size_t ps_n = 0;
  unsigned char *buffer = NULL;
  size_t buffer_n = 0;
  unsigned char *asn = NULL;
  size_t asn_n = 0;
  unsigned int i = 0;
  
  /* Create hashing handle and get the necessary information.  */
  err = _gcry_md_open (&handle, options->md, 0);
  if (! err)
    _gcry_md_info_get (handle, &asn, &asn_n, &h_n);

  if (! err)
    {
      /* Apply the hash function to the message M to produce a hash
	 value H.  */
      gcry_md_write (handle, m, m_n);
      if (! err)
	err = gcry_md_final (handle);
      if (! err)
	h = gcry_md_read (handle, 0);
    }

  if (! err)
    {
      /* Encode the algorithm ID for the hash function and the hash
	 value into an ASN.1 value of type DigestInfo with the
	 Distinguished Encoding Rules (DER), where the type DigestInfo
	 has the syntax: 

	 DigestInfo ::== SEQUENCE {
	     digestAlgorithm AlgorithmIdentifier,
	     digest OCTET STRING
	 }

	 The first field identifies the hash function and the second
	 contains the hash value.  Let T be the DER encoding of the
	 DigestInfo value and let tLen be the length in octets of
	 T.  */

      t_n = asn_n + h_n;
      t = gcry_malloc (t_n);
      if (! t)
	err = gpg_err_code_from_errno (ENOMEM);
      else
	{
	  for (i = 0; i < asn_n; i++)
	    t[i] = asn[i];
	  for (i = 0; i < h_n; i++)
	    t[asn_n + i] = h[i];
	}
    }

  if (! err)
    /* If emLen < tLen + 11, output "intended encoded message length
       too short" and stop.  */
    if (options->em_n < t_n + 11)
      err = GPG_ERR_TOO_SHORT;

  
  if (! err)
    {
      /* Generate an octet string PS consisting of emLen - tLen - 3
	 octets with hexadecimal value 0xFF.  The length of PS will be
	 at least 8 octets.  */
      ps_n = options->em_n - t_n - 3;
      ps = gcry_malloc (ps_n);
      if (! ps)
	err = gpg_err_code_from_errno (ENOMEM);
      else
	for (i = 0; i < ps_n; i++)
	  ps[i] = 0xFF;
    }

  if (! err)
    {
      /* Concatenate PS, the DER encoding T, and other padding to form
	 the encoded message EM as:

	 EM = 0x00 || 0x01 || PS || 0x00 || T.  */

      buffer_n = ps_n + t_n + 3;
      buffer = gcry_malloc (buffer_n);
      if (! buffer)
	err = gpg_err_code_from_errno (ENOMEM);
      else
	{
	  buffer[0] = 0x00;
	  buffer[1] = 0x01;
	  for (i = 0; i < ps_n; i++)
	    buffer[2 + i] = ps[i];
	  buffer[2 + ps_n] = 0x00;
	  for (i = 0; i < t_n; i++)
	    buffer[3 + ps_n + i] = t[i];
	}
    }

  if (handle)
    gcry_md_close (handle);

  if (t)
    gcry_free (t);
  if (ps)
    gcry_free (ps);

  if (! err)
    {
      *em = buffer;
      *em_n = buffer_n;
    }
  
  return err;
}

/* Encode a message according to the Encoding Method for Encryption
   `PKCS-V1_5' (EME-PKCS-V1_5).  */
static gcry_err_code_t
eme_pkcs_v1_5_encode (unsigned int flags, void *opts,
		      unsigned char *m, size_t m_n,
		      unsigned char **em, size_t *em_n)
{
  gcry_ac_eme_pkcs_v1_5_t *options = (gcry_ac_eme_pkcs_v1_5_t *) opts;
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  unsigned char *buffer = NULL;
  unsigned char *ps = NULL;
  unsigned int ps_n = 0;
  unsigned int k = 0;

  /* FIXME, confusion of bits and bytes.  */

  err = _gcry_ac_key_get_nbits (options->handle, options->key, &k);
  if (! err)
    {
      k /= 8;
      if (m_n > k - 11)
	/* Key is too short for message.  */
	err = GPG_ERR_TOO_SHORT;
    }

  if (! err)
    {
      /* Allocate buffer.  */
      buffer = gcry_malloc (k);
      if (! buffer)
	err = gpg_err_code_from_errno (ENOMEM);
    }

  if (! err)
    {
      /* Generate an octet string PS of length k - mLen - 3 consisting
	 of pseudorandomly generated nonzero octets.  The length of PS
	 will be at least eight octets.  */
      ps_n = k - m_n - 3;
      ps = buffer + 2;
      em_randomize_nonzero (ps, ps_n, GCRY_STRONG_RANDOM);
      {
	unsigned int a;
	for (a = 0; a < ps_n; a++)
	  assert (ps[a]);
      }
    }

  if (! err)
    {
      /* Concatenate PS, the message M, and other padding to form an
	 encoded message EM of length k octets as:

	 EM = 0x00 || 0x02 || PS || 0x00 || M.  */

      buffer[0] = 0x00;
      buffer[1] = 0x02;
      buffer[ps_n + 2] = 0x00;
      memcpy (buffer + ps_n + 3, m, m_n);
    }

  if (! err)
    {
      *em = buffer;
      *em_n = k;
    }

  return err;
}

/* Decode a message according to the Encoding Method for Encryption
   `PKCS-V1_5' (EME-PKCS-V1_5).  */
static gcry_err_code_t
eme_pkcs_v1_5_decode (unsigned int flags, void *opts,
		      unsigned char *em, size_t em_n,
		      unsigned char **m, size_t *m_n)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  unsigned char *buffer = NULL;
  unsigned int i = 0;

  if (! ((em_n >= 12) && (em[0] == 0x00) && (em[1] == 0x02)))
    err = GPG_ERR_TOO_SHORT;
  else
    {
      for (i = 2; (i < em_n) && em[i]; i++);
      i++;

      if ((i == em_n) || ((i - 2) < 8))
	err = GPG_ERR_INTERNAL;	/* FIXME.  */
    }

  if (! err)
    {
      buffer = gcry_malloc (em_n - i);
      if (! buffer)
	err = gpg_err_code_from_errno (ENOMEM);
    }

  if (! err)
    {
      memcpy (buffer, em + i, em_n - i);
      *m = buffer;
      *m_n = em_n - i;
    }

  return err;
}

/* `Actions' for data_dencode().  */
typedef enum dencode_action
  {
    DATA_ENCODE,
    DATA_DECODE,
  }
dencode_action_t;

/* Encode or decode a message according to the the encoding method
   METHOD; ACTION specifies wether the message that is contained in
   BUFFER_IN and of length BUFFER_IN_N should be encoded or decoded.
   The resulting message will be stored in a newly allocated buffer in
   BUFFER_OUT and BUFFER_OUT_N.  */
static gcry_err_code_t
data_dencode (gcry_ac_em_t method, dencode_action_t action,
	      unsigned int flags, void *options,
	      unsigned char *buffer_in, size_t buffer_in_n,
	      unsigned char **buffer_out, size_t *buffer_out_n)
{
  gcry_err_code_t err = GPG_ERR_INTERNAL; /* FIXME; UNKNOWN_ENCODING_METHOD.  */
  struct
  {
    gcry_ac_em_t method;
    gcry_ac_em_dencode_t encode;
    gcry_ac_em_dencode_t decode;
  } methods[] =
    {
      { GCRY_AC_EME_PKCS_V1_5,
	eme_pkcs_v1_5_encode, eme_pkcs_v1_5_decode },
      { GCRY_AC_EMSA_PKCS_V1_5,
	emsa_pkcs_v1_5_encode, NULL },
    };
  unsigned int i = 0;

  for (i = 0; i < (sizeof (methods) / sizeof (*methods)); i++)
    {
      if (methods[i].method == method)
	{
	  switch (action)
	    {
	    case DATA_ENCODE:
	      if (methods[i].encode)
		err = (*methods[i].encode) (flags, options,
					    buffer_in, buffer_in_n,
					    buffer_out, buffer_out_n);
	      else
		err = GPG_ERR_INTERNAL;	/* FIXME.  */
	      break;
	      
	    case DATA_DECODE:
	      if (methods[i].decode)	      
		err = (*methods[i].decode) (flags, options,
					    buffer_in, buffer_in_n,
					    buffer_out, buffer_out_n);
	      else
		err = GPG_ERR_INTERNAL; /* FIXME.  */
	      break;
	    }
	  break;
	}
    }

  return err;
}

/* Encode a message according to the encoding method METHOD.  OPTIONS
   must be a pointer to a method-specific structure
   (gcry_ac_em*_t).  */
gcry_error_t
gcry_ac_data_encode (gcry_ac_em_t method,
		     unsigned int flags, void *options,
		     unsigned char *m, size_t m_n,
		     unsigned char **em, size_t *em_n)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = data_dencode (method, DATA_ENCODE, flags, options,
		      m, m_n, em, em_n);

  return gcry_error (err);
}

/* Dencode a message according to the encoding method METHOD.  OPTIONS
   must be a pointer to a method-specific structure
   (gcry_ac_em*_t).  */
gcry_error_t
gcry_ac_data_decode (gcry_ac_em_t method,
		     unsigned int flags, void *options,
		     unsigned char *em, size_t em_n,
		     unsigned char **m, size_t *m_n)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = data_dencode (method, DATA_DECODE, flags, options,
		      em, em_n, m, m_n);

  return gcry_error (err);
}

/* Convert an MPI into an octet string.  */
void
_gcry_ac_mpi_to_os (gcry_mpi_t mpi, unsigned char *os, size_t os_n)
{
  gcry_mpi_t base = NULL, m = NULL, d = NULL;
  unsigned int n = 0, i = 0;
  unsigned long digit = 0;

  base = gcry_mpi_new (0);
  gcry_mpi_set_ui (base, 256);

  m = gcry_mpi_copy (mpi);
  while (gcry_mpi_cmp_ui (m, 0))
    {
      n++;
      gcry_mpi_div (m, NULL, m, base, 0);
    }

  gcry_mpi_set (m, mpi);
  d = gcry_mpi_new (0);
  for (i = 0; (i < n) && (i < os_n); i++)
    {
      gcry_mpi_mod (d, m, base);
      _gcry_mpi_get_ui (d, &digit);
      gcry_mpi_div (m, NULL, m, base, 0);
      os[os_n - i - 1] = (digit & 0xFF);
    }

  for (; i < os_n; i++)
    os[os_n - i - 1] = 0;

  gcry_mpi_release (base);
  gcry_mpi_release (d);
  gcry_mpi_release (m);
}

/* Convert an MPI into an octet string (I2OSP). */
void
gcry_ac_mpi_to_os (gcry_mpi_t mpi, unsigned char *os, size_t os_n)
{
  return _gcry_ac_mpi_to_os (mpi, os, os_n);
}

/* Convert an MPI into an newly allocated octet string.  */
static gcry_err_code_t
_gcry_ac_mpi_to_os_alloc (gcry_mpi_t mpi, unsigned char **os, size_t *os_n)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  unsigned char *buffer = NULL;
  size_t buffer_n = 0;
  unsigned int nbits = 0;

  nbits = gcry_mpi_get_nbits (mpi);
  buffer_n = (nbits + 7) / 8;
  buffer = gcry_malloc (buffer_n);
  if (! buffer)
    err = gpg_err_code_from_errno (ENOMEM);

  if (! err)
    {
      _gcry_ac_mpi_to_os (mpi, buffer, buffer_n);
      *os = buffer;
      *os_n = buffer_n;
    }

  return err;
}

/* Convert an MPI into an newly allocated octet string.  */
gcry_error_t
gcry_ac_mpi_to_os_alloc (gcry_mpi_t mpi, unsigned char **os, size_t *os_n)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = _gcry_ac_mpi_to_os_alloc (mpi, os, os_n);

  return gcry_error (err);
}

/* Convert an octet string into an MPI.  */
void
_gcry_ac_os_to_mpi (gcry_mpi_t mpi, unsigned char *os, size_t os_n)
{
  gcry_mpi_t a = NULL, xi = NULL, x = NULL;
  unsigned int i = 0;
  
  a = gcry_mpi_new (0);
  gcry_mpi_set_ui (a, 1);
  x = gcry_mpi_new (0);
  gcry_mpi_set_ui (x, 0);
  xi = gcry_mpi_new (0);

  for (i = 0; i < os_n; i++)
    {
      gcry_mpi_mul_ui (xi, a, os[os_n - i - 1]);
      gcry_mpi_add (x, x, xi);
      gcry_mpi_mul_ui (a, a, 256);
    }
      
  gcry_mpi_release (a);
  gcry_mpi_release (xi);
  gcry_mpi_set (mpi, x);
}

/* Convert an octet string into an MPI (OS2IP).  */
void
gcry_ac_os_to_mpi (gcry_mpi_t mpi, unsigned char *os, size_t os_n)
{
  return _gcry_ac_os_to_mpi (mpi, os, os_n);
}

/* 
 * Implementation of Encryption Schemes (ES) and Signature Schemes
 * with Appendix (SSA).
 */


typedef gcry_err_code_t (*gcry_ac_dencode_prepare_t) (gcry_ac_handle_t handle, gcry_ac_key_t key,
						      void *opts, void *opts_em);

typedef gcry_err_code_t (*gcry_ac_dencode_to_os_t) (gcry_ac_handle_t handle,
						    gcry_ac_key_t key, void *opts,
						    gcry_mpi_t mpi,
						    unsigned char **os, size_t *os_n);

static gcry_err_code_t
ac_es_dencode_prepare_pkcs_v1_5 (gcry_ac_handle_t handle, gcry_ac_key_t key,
				 void *opts, void *opts_em)
{
  gcry_ac_eme_pkcs_v1_5_t *options_em = (gcry_ac_eme_pkcs_v1_5_t *) opts_em;
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  options_em->handle = handle;
  options_em->key = key;

  return err;
}

static gcry_err_code_t
ac_es_dencode_to_os_pkcs_v1_5 (gcry_ac_handle_t handle, gcry_ac_key_t key, void *opts,
			       gcry_mpi_t mpi, unsigned char **os, size_t *os_n)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  unsigned char *buffer = NULL;
  unsigned char buffer_n = 0;
  unsigned int k = 0;

  err = _gcry_ac_key_get_nbits (handle, key, &k);
  if (! err)
    {
      buffer_n = (k + 7) / 8;
      buffer = gcry_malloc (buffer_n);
      if (! buffer)
	err = gpg_err_code_from_errno (ENOMEM);
    }
  if (! err)
    gcry_ac_mpi_to_os (mpi, buffer, buffer_n);

  if (! err)
    {
      *os = buffer;
      *os_n = buffer_n;
    }

  return err;
}

static gcry_err_code_t
ac_ssa_dencode_prepare_pkcs_v1_5 (gcry_ac_handle_t handle, gcry_ac_key_t key,
				  void *opts, void *opts_em)
{
  gcry_ac_emsa_pkcs_v1_5_t *options_em = (gcry_ac_emsa_pkcs_v1_5_t *) opts_em;
  gcry_ac_ssa_pkcs_v1_5_t *options = (gcry_ac_ssa_pkcs_v1_5_t *) opts;
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  unsigned int k = 0;

  err = _gcry_ac_key_get_nbits (handle, key, &k);
  if (! err)
    {
      k = (k + 7) / 8;
      options_em->md = options->md;
      options_em->em_n = k;
    }

  return err;
}

typedef struct ac_scheme
{
  gcry_ac_scheme_t scheme;
  gcry_ac_em_t scheme_encoding;
  gcry_ac_dencode_prepare_t dencode_prepare;
  gcry_ac_dencode_to_os_t dencode_to_os;
  size_t options_em_n;
} ac_scheme_t;

static ac_scheme_t ac_schemes[] =
  {
    { GCRY_AC_ES_PKCS_V1_5, GCRY_AC_EME_PKCS_V1_5,
      ac_es_dencode_prepare_pkcs_v1_5, ac_es_dencode_to_os_pkcs_v1_5,
      sizeof (gcry_ac_eme_pkcs_v1_5_t) },
    { GCRY_AC_SSA_PKCS_V1_5, GCRY_AC_EMSA_PKCS_V1_5,
      ac_ssa_dencode_prepare_pkcs_v1_5, NULL,
      sizeof (gcry_ac_emsa_pkcs_v1_5_t) }
  };

static gcry_err_code_t
ac_scheme_get (gcry_ac_scheme_t scheme, ac_scheme_t *ac_scheme)
{
  gcry_err_code_t err = GPG_ERR_INTERNAL; /* FIXME: UNKNOWN_SCHEME.  */
  unsigned int i = 0;

  for (i = 0; i < (sizeof (ac_schemes) / sizeof (*ac_schemes)); i++)
    if (scheme == ac_schemes[i].scheme)
      {
	err = GPG_ERR_NO_ERROR;
	*ac_scheme = ac_schemes[i];
	break;
      }
  
  return err;
}

static gcry_err_code_t
ac_dencode_prepare (gcry_ac_handle_t handle, gcry_ac_key_t key, void *opts,
		    ac_scheme_t scheme, void **opts_em)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  void *options_em = NULL;

  options_em = gcry_malloc (scheme.options_em_n);
  if (! options_em)
    err = gpg_err_code_from_errno (ENOMEM);

  if (! err)
    err = (*scheme.dencode_prepare) (handle, key, opts, options_em);
  
  if (! err)
    *opts_em = options_em;

  if (err)
    {
      if (options_em)
	free (options_em);
    }

  return err;
}

static gcry_err_code_t
ac_es_dencode_to_os (gcry_ac_handle_t handle, gcry_ac_key_t key, void *opts,
		     ac_scheme_t scheme, gcry_mpi_t mpi, unsigned char **os, size_t *os_n)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;

  err = (*scheme.dencode_to_os) (handle, key, opts, mpi, os, os_n);

  return err;
}

typedef enum ac_scheme_data_type
  {
    DATA_TYPE_ENCRYPTED,
    DATA_TYPE_SIGNED,
  }
ac_scheme_data_type_t;
    
static gcry_err_code_t
ac_data_set_to_mpi (gcry_ac_handle_t handle, ac_scheme_data_type_t type,
		    gcry_ac_data_t data, gcry_mpi_t *mpi)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_mpi_t mpi_new = NULL;
  const char *name = NULL;
  unsigned int elems = 0;

  switch (type)
    {
    case DATA_TYPE_ENCRYPTED:
      elems = ((gcry_ac_spec_t *) handle->module->spec)->elems_data_encrypted;
      break;
    case DATA_TYPE_SIGNED:
      elems = ((gcry_ac_spec_t *) handle->module->spec)->elems_data_signed;
      break;
    }

  if (elems != 1)
    err = GPG_ERR_CONFLICT;
  else
    {
      switch (type)
	{
	case DATA_TYPE_ENCRYPTED:
	  name = ((gcry_ac_spec_t *) handle->module->spec)->spec_data_encrypted[0].name;
	  break;
	case DATA_TYPE_SIGNED:
	  name = ((gcry_ac_spec_t *) handle->module->spec)->spec_data_signed[0].name;
	  break;
	}
      
      err = _gcry_ac_data_get_name (data, name, &mpi_new);
    }

  if (! err)
    *mpi = mpi_new;

  return err;
}

static gcry_err_code_t
ac_mpi_to_data_set (gcry_ac_handle_t handle, ac_scheme_data_type_t type,
		    gcry_ac_data_t *data, gcry_mpi_t mpi)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_data_t data_new = NULL;
  const char *name = NULL;
  unsigned int elems = 0;

  switch (type)
    {
    case DATA_TYPE_ENCRYPTED:
      elems = ((gcry_ac_spec_t *) handle->module->spec)->elems_data_encrypted;
      break;
    case DATA_TYPE_SIGNED:
      elems = ((gcry_ac_spec_t *) handle->module->spec)->elems_data_signed;
      break;
    }

  if (elems != 1)
    err = GPG_ERR_CONFLICT;
  else
    {
      err = _gcry_ac_data_new (&data_new);
      if (! err)
	{
	  switch (type)
	    {
	    case DATA_TYPE_ENCRYPTED:
	      name = ((gcry_ac_spec_t *) handle->module->spec)->spec_data_encrypted[0].name;
	      break;
	    case DATA_TYPE_SIGNED:
	      name = ((gcry_ac_spec_t *) handle->module->spec)->spec_data_signed[0].name;
	      break;
	    }

	  err = _gcry_ac_data_set (data_new, name, mpi);
	}
    }

  if (! err)
    *data = data_new;

  return err;
}

gcry_error_t
gcry_ac_data_encrypt_scheme (gcry_ac_handle_t handle, gcry_ac_scheme_t scheme_id,
			     unsigned int flags, void *opts,
			     gcry_ac_key_t key_public,
			     unsigned char *m, size_t m_n,
			     unsigned char **c, size_t *c_n)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  unsigned char *em = NULL;
  size_t em_n = 0;
  gcry_mpi_t mpi_plain = NULL;
  gcry_ac_data_t data_encrypted = NULL;
  gcry_mpi_t mpi_encrypted = NULL;
  unsigned char *buffer = NULL;
  size_t buffer_n = 0;
  void *opts_em = NULL;
  ac_scheme_t scheme;

  err = ac_scheme_get (scheme_id, &scheme);
  if (! err)
    err = ac_dencode_prepare (handle, key_public, opts, scheme, &opts_em);
  if (! err)
    err = data_dencode (scheme.scheme_encoding, DATA_ENCODE, 0, opts_em, m, m_n, &em, &em_n);
  if (! err)
    {
      mpi_plain = gcry_mpi_snew (0);
      gcry_ac_os_to_mpi (mpi_plain, em, em_n);
    }

  if (! err)
    err = _gcry_ac_data_encrypt (handle, 0, key_public, mpi_plain, &data_encrypted);

  if (! err)
    err = ac_data_set_to_mpi (handle, DATA_TYPE_ENCRYPTED, data_encrypted, &mpi_encrypted);

  if (! err)
    _gcry_ac_mpi_to_os_alloc (mpi_encrypted, &buffer, &buffer_n);

  if (opts_em)
    gcry_free (opts_em);
  if (mpi_plain)
    gcry_mpi_release (mpi_plain);
  if (em)
    gcry_free (em);
  if (data_encrypted)
    gcry_ac_data_destroy (data_encrypted);
  
  if (! err)
    {
      *c = buffer;
      *c_n = buffer_n;
    }

  return gcry_error (err);
}

gcry_error_t
gcry_ac_data_decrypt_scheme (gcry_ac_handle_t handle, gcry_ac_scheme_t scheme_id,
			     unsigned int flags, void *opts,
			     gcry_ac_key_t key_secret,
			     unsigned char *c, size_t c_n,
			     unsigned char **m, size_t *m_n)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_data_t data_encrypted = NULL;
  unsigned char *em = NULL;
  size_t em_n = 0;
  gcry_mpi_t mpi_encrypted = NULL;
  gcry_mpi_t mpi_decrypted = NULL;
  unsigned char *buffer = NULL;
  size_t buffer_n = 0;
  void *opts_em = NULL;
  ac_scheme_t scheme;

  err = ac_scheme_get (scheme_id, &scheme);
  if (! err)
    {
      mpi_encrypted = gcry_mpi_snew (0);
      gcry_ac_os_to_mpi (mpi_encrypted, c, c_n);
    }
  if (! err)
    err = ac_mpi_to_data_set (handle, DATA_TYPE_ENCRYPTED, &data_encrypted, mpi_encrypted);
  if (! err)
    err = _gcry_ac_data_decrypt (handle, 0, key_secret, &mpi_decrypted, data_encrypted);
  if (! err)
    ac_es_dencode_to_os (handle, key_secret, opts, scheme, mpi_decrypted, &em, &em_n);
  if (! err)
    err = ac_dencode_prepare (handle, key_secret, opts, scheme, &opts_em);
  if (! err)
    err = data_dencode (scheme.scheme_encoding, DATA_DECODE, 0,
			opts_em, em, em_n, &buffer, &buffer_n);

  if (opts_em)
    gcry_free (opts_em);
  if (data_encrypted)
    _gcry_ac_data_destroy (data_encrypted);
  if (mpi_encrypted)
    gcry_mpi_release (mpi_encrypted);
  if (mpi_decrypted)
    gcry_mpi_release (mpi_decrypted);
  if (em)
    gcry_free (em);

  if (! err)
    {
      *m = buffer;
      *m_n = buffer_n;
    }

  return gcry_error (err);
}

gcry_error_t
gcry_ac_data_sign_scheme (gcry_ac_handle_t handle, gcry_ac_scheme_t scheme_id,
			  unsigned int flags, void *opts, gcry_ac_key_t key_secret,
			  unsigned char *m, size_t m_n, unsigned char **s, size_t *s_n)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_data_t data_signed = NULL;
  unsigned char *em = NULL;
  size_t em_n = 0;
  gcry_mpi_t mpi = NULL;
  void *opts_em = NULL;
  unsigned char *buffer = NULL;
  size_t buffer_n = 0;
  gcry_mpi_t mpi_signed = NULL;
  ac_scheme_t scheme;

  err = ac_scheme_get (scheme_id, &scheme);
  if (! err)
    err = ac_dencode_prepare (handle, key_secret, opts, scheme, &opts_em);
  if (! err)
    err = data_dencode (scheme.scheme_encoding, DATA_ENCODE, 0, opts_em, m, m_n, &em, &em_n);
  if (! err)
    {
      mpi = gcry_mpi_new (0);
      _gcry_ac_os_to_mpi (mpi, em, em_n);
    }
  if (! err)
    err = _gcry_ac_data_sign (handle, key_secret, mpi, &data_signed);
  if (! err)
    err = ac_data_set_to_mpi (handle, DATA_TYPE_SIGNED, data_signed, &mpi_signed);
  if (! err)
    err = _gcry_ac_mpi_to_os_alloc (mpi_signed, &buffer, &buffer_n);

  if (opts_em)
    gcry_free (opts_em);
  if (em)
    gcry_free (em);
  if (mpi)
    gcry_mpi_release (mpi);
  if (data_signed)
    _gcry_ac_data_destroy (data_signed);
  
  if (! err)
    {
      *s = buffer;
      *s_n = buffer_n;
    }

  return gcry_error (err);
}

gcry_error_t
gcry_ac_data_verify_scheme (gcry_ac_handle_t handle, gcry_ac_scheme_t scheme_id,
			    unsigned int flags, void *opts, gcry_ac_key_t key_public,
			    unsigned char *m, size_t m_n, unsigned char *s, size_t s_n)
{
  gcry_err_code_t err = GPG_ERR_NO_ERROR;
  gcry_ac_data_t data_signed = NULL;
  unsigned char *em = NULL;
  size_t em_n = 0;
  void *opts_em = NULL;
  gcry_mpi_t mpi_signature = NULL;
  gcry_mpi_t mpi_data = NULL;
  ac_scheme_t scheme;

  err = ac_scheme_get (scheme_id, &scheme);
  if (! err)
    err = ac_dencode_prepare (handle, key_public, opts, scheme, &opts_em);
  if (! err)
    {
      mpi_signature = gcry_mpi_new (0);
      _gcry_ac_os_to_mpi (mpi_signature, s, s_n);
    }
  if (! err)
    err = ac_mpi_to_data_set (handle, DATA_TYPE_SIGNED, &data_signed, mpi_signature);
  if (! err)
    err = data_dencode (scheme.scheme_encoding, DATA_ENCODE, 0,
			opts_em, m, m_n, &em, &em_n);
  if (! err)
    {
      mpi_data = gcry_mpi_new (0);
      _gcry_ac_os_to_mpi (mpi_data, em, em_n);
    }
  if (! err)
    err = _gcry_ac_data_verify (handle, key_public, mpi_data, data_signed);

  if (opts_em)
    gcry_free (opts_em);
  if (mpi_signature)
    gcry_mpi_release (mpi_signature);
  if (data_signed)
    _gcry_ac_data_destroy (data_signed);
  if (em)
    gcry_free (em);
  if (mpi_data)
    gcry_mpi_release (mpi_data);

  return err;
}
