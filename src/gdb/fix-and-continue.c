/* Fix and Continue support for gdb

   Copyright 2003 Free Software Foundation, Inc.

   Contributed by Apple Computer, Inc.
   Written by Jason Molenda.
   Inspired by Fix & Continue implementation in wdb by Hewlett-Packard Company.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

#include <string.h>
#include <mach-o/dyld.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <stdlib.h>
#include <stdint.h>

#include "defs.h"
#include "value.h"
#include "gdbtypes.h"
#include "objfiles.h"
#include "command.h"
#include "completer.h"
#include "frame.h"
#include "target.h"
#include "gdbcore.h"
#include "inferior.h"
#include "symfile.h"
#include "gdbthread.h"
#include "gdb.h"
#include "ui-out.h"
#include "cli-out.h"
#include "symtab.h"
#include "regcache.h"
#include "gdbcmd.h"
#include "language.h"
#include <readline/readline.h>

#if defined(TARGET_POWERPC)
#include "ppc-macosx-frameinfo.h"
#include "ppc-macosx-tdep.h"
#endif /* TARGET_POWERPC */

#include "macosx-nat-dyld-process.h"


/* A list of all active threads, and the functions those threads
   have currently executing which are in the fixed object file.
   This information is only useful at the point of fix-up, where
   we're looking for restriction violations and reporting the
   state of the stack to the UI.  */

struct active_threads {
  int num;
  struct active_func *active_func_chain;
  CORE_ADDR pc;
  struct active_threads *next;
};

/* The structure for a single function that is active at the
   time of the fix request. */

struct active_func {
  struct symbol * sym;
  struct active_func * next;
  struct frame_info *fi;
};

/* Keep track of all inferior data we change while adding in a fixed
   .o file, so that we can restore the state of the program if a fix
   is aborted half way (due to a restriction violation, for example).
   The Apple implementation of this does all of its syntax checking
   before loading the file, so this isn't nearly as necessary.  In
   fact, I don't think it's of any use to me at all.  */

struct fixeddatum {
  CORE_ADDR addr;
  int size;
  int oldval, newval;       /* old and new values of the datum */
  struct fixeddatum * next;
};

/* Keep track of all symbols we mark obsolete while adding in a fixed
   .o file, so that we can restore the state of the program if a fix
   is aborted half way (due to a restriction violation, for example).
   The Apple implementation of this does all of its syntax checking
   before loading the file, so this isn't nearly as necessary.  In
   fact, I don't think it's of any use to me at all.  */

struct obsoletedsym {
  struct minimal_symbol * oldmsym, *newmsym;
  struct symbol * oldsym, *newsym;
  struct obsoletedsym * next;
};

/* Data structure to keep track of files being fixed.  This primarily
   acts as the token o' data that is passed around all the functions
   as we're handling a fix request.  */

struct fixinfo {

  /* Source, bundle, and object filenames.  Probably unnecessary to store both
     full name and the basename, but we seem to be recomputing that 
     in several places, so I'll stash it here.  The "basename" variants
     are just pointers into the middle of the "filename" malloced memory;
     only free the "filename" versions. 

     The object filenames ("foo.o") are used only to communicate with ZeroLink,
     which only knows about object files.

     Note that we have one FIXINFO struct for each *SOURCE* file, but
     each fixed bundl3 will have a different name.  The bundle_filename
     will contain the most recently seen bundle filename.  */

  const char *src_filename;
  const char *src_basename;
  const char *bundle_filename;
  const char *bundle_basename;
  const char *object_filename;

  /* The original objfile (original_objfile_filename) and source file
     (canonical_source_filename) that this fixinfo structure represents.

     canonical_source_filename is a pointer to either src_filename or
     src_basename, depending on which name is found in the executable
     objfile's psymtabs/symtabs.  */

  const char *original_objfile_filename;
  const char *canonical_source_filename;

  /* The list of active functions is only useful at the point where
     the fix request comes in -- once that request has been completed,
     this can be xfree()'ed. */

  struct active_threads *active_functions;

  /* The chain of fixed versions of this object file (when the user fixes
     the same .o multiple times.)  */

  struct fixedobj *fixed_object_files;

  /* The most recently fixed .o file we've loaded for this file (i.e.
     a user "fixes" a single source file multiple times; this stores
     the current version.)  

     NB: This is really just the tail of the fixed_object_files
     linked list, but the HP implementation did their link lists
     the opposite way so I want to make it clear where the most
     recent objfile is.  */

  struct fixedobj *most_recent_fix;

  /* The fixinfo structure is built up as we're doing the initial scan
     of the proposed object file, and if we error out half-way through,
     we need to recognize a half-finished structure.  

     FIXME: I should do something cleaner here, but a simple cleanup
     to remove the fixinfo struct in the case of an error would be
     incorrect if the fixinfo structure had already existed.  Note
     the directly related function free_half_finished_fixinfo (). */

  int complete;

  /* One of these structures for each object file the user tries to fix. */

  struct fixinfo *next;
};

/* Data structure to track each fixed object file (.o) that we pull
   in.  There may be man copies of a single object file loaded into
   a program; each of them will have one of these structures and a
   single struct fixinfo to hold all fixes to that object file. */

struct fixedobj {

  struct objfile *objfile;

  /* Bundle file name, including path */

  const char *bundle_filename;

  /* Maintain a chain of inferior data we modified in the process
     of installing this object file so we can, in theory, back the
     fix out. */

  struct fixeddatum *firstdatum, *lastdatum;

  /* Maintain a list of symbols we declared obsolete while installing
     this object file so we can, in theory, back the fix out. */

  struct obsoletedsym *obsoletedsym;

  struct fixedobj *next;
};

static struct fixinfo *fixinfo_chain = NULL;


/* References to static/global data allocated in the new CU need to be
   redirected to the original locations.  An array of these structures
   are populated with the address of an entry in the indirection table
   of the just-loaded CU, the destination address that that entry originally
   held (and which gdb will be redirecting), a pointer to the new symbol
   it is pointing to, and a pointer to the original symbol that it needs to
   be pointing to.  */

struct file_static_fixups {
  CORE_ADDR addr;
  CORE_ADDR value;
  struct symbol *new_sym;
  struct minimal_symbol *new_msym;
  struct symbol *original_sym;
  struct minimal_symbol *original_msym;
};



static void get_fixed_file (struct fixinfo *);

static int load_fixed_objfile (const char *);

static void do_final_fix_fixups (struct fixinfo *curfixinfo);

static void redirect_file_statics (struct fixinfo *);

static void find_new_static_symbols (struct fixinfo *, struct file_static_fixups *, int);

static void find_orig_static_symbols (struct fixinfo *, struct file_static_fixups *, int);

static void redirect_statics (struct file_static_fixups *, int);

static int find_and_parse_nonlazy_ptr_sect (struct fixinfo *, struct file_static_fixups **);

static struct objfile ** build_list_of_objfiles_to_update (struct fixinfo *);

static void redirect_old_function (struct fixinfo *, struct symbol *, struct symbol *, int);

CORE_ADDR decode_fix_and_continue_trampoline (CORE_ADDR);

static uint16_t encode_lo16 (CORE_ADDR);
static uint16_t encode_hi16 (CORE_ADDR);

static CORE_ADDR decode_hi16_lo16 (uint16_t, uint16_t);

static int in_active_func (const char *, struct active_threads *);

static struct active_func *create_current_active_funcs_list (const char *);

void do_final_fix_fixups_global_syms (struct block *newglobals, struct objfile *oldobj, struct fixinfo *curfixinfo);

static void do_final_fix_fixups_static_syms (struct block *newstatics, struct objfile *oldobj, struct fixinfo *curfixinfo);

static struct objfile ** build_list_of_current_objfiles (void);

static struct objfile *find_newly_added_objfile (struct objfile **, const char *);

static void pre_load_and_check_file (struct fixinfo *);

static void force_psymtab_expansion (struct objfile *, const char *, const char *);

static void expand_all_objfile_psymtabs (struct objfile *);

static void free_active_threads_struct (struct active_threads *);

static struct active_threads * create_current_threads_list (const char *);

static struct fixinfo *get_fixinfo_for_new_request (const char *);

static int file_exists_p (const char *);

static void do_pre_load_checks (struct fixinfo *, struct objfile *);

static void check_restrictions_globals (struct fixinfo *, struct objfile *);

static void check_restrictions_statics (struct fixinfo *, struct objfile *);

static void check_restrictions_locals (struct fixinfo *, struct objfile *);

static void check_restrictions_function (const char *, int, struct block *, 
                                         struct block *);

static void check_restriction_cxx_zerolink (struct objfile *);

static int sym_is_argument (struct symbol *);

static int sym_is_local (struct symbol *);

static void free_half_finished_fixinfo (struct fixinfo *);

static void mark_previous_fixes_obsolete (struct fixinfo *);

static void fix_command_helper (const char *, const char *, const char *);

static const char *getbasename (const char *);

static int inferior_is_zerolinked_p (void);

static void tell_zerolink (struct fixinfo *);

static void print_active_functions (struct fixinfo *);

static struct objfile *find_objfile_by_name (const char *);

static struct symbol *search_for_coalesced_symbol (struct objfile *, 
                                                   struct symbol *);

static void restore_language (void *);

static struct cleanup *set_current_language (const char *);

static void find_original_object_file_name (struct fixinfo *);

static struct objfile *find_original_object_file (struct fixinfo *);

static struct symtab *find_original_symtab (struct fixinfo *);

static struct partial_symtab *find_original_psymtab (struct fixinfo *);



int fix_and_continue_debug_flag = 0;

#ifndef TARGET_ADDRESS_BYTES
#define TARGET_ADDRESS_BYTES (TARGET_LONG_BIT / TARGET_CHAR_BIT)
#endif

static void
fix_command (char *args, int from_tty)
{
  char **argv;
  char *filename;
  char *object_filename, *source_filename, *bundle_filename;
  char tmpbuf[MAXPATHLEN];
  struct cleanup *cleanups;
  const char *usage = "Usage: fix bundle-filename source-filename [object-filename]";

  if (!args || args[0] == '\0')
    error (usage);

  argv = buildargv (args);
  cleanups = make_cleanup_freeargv (argv);

  /* Two required arguments.  */

  if (argv[0] == NULL || strlen (argv[0]) == 0 ||
      argv[1] == NULL || strlen (argv[1]) == 0)
    error (usage);

  /* An optional third argument.  */

  if (argv[2] != NULL && 
      (strlen (argv[2]) == 0 || argv[3] != NULL))
    error (usage);

  /* Get first argument: Bundle file name */

  filename = tilde_expand (argv[0]);
  realpath (filename, tmpbuf);
  bundle_filename = (char *) xmalloc (strlen (tmpbuf) + 1);
  strcpy (bundle_filename, tmpbuf);
  xfree (filename);

  /* Get second argument:  Source file name */
  
  source_filename = tilde_expand (argv[1]);

  if (!source_filename || strlen (source_filename) == 0 ||
      !bundle_filename || strlen (bundle_filename) == 0)
    error (usage);

  /* Get third argument:  Object file name (only needed for ZeroLink) */

  if (argv[2] != NULL)
    object_filename = tilde_expand (argv[2]);
  else
    object_filename = NULL;

  fix_command_helper (source_filename, bundle_filename, object_filename);

  if (!ui_out_is_mi_like_p (uiout) && from_tty)
    {
      printf_filtered ("Fix succeeded.\n");
    }
      
  do_cleanups (cleanups);
}


/* All filename arguments should be tilde expanded, and the bundle
   filename should be run through realpath() before getting here so
   it's the same form that dyld will report.  */

static void
fix_command_helper (const char *source_filename,
                    const char *bundle_filename,
                    const char *object_filename)
{
  struct fixinfo *cur;
  struct cleanup *wipe;

  if (!file_exists_p (source_filename))
    error ("Source file '%s' not found.", source_filename);

  if (!file_exists_p (bundle_filename))
    error ("Bundle '%s' not found.", bundle_filename);

  if (object_filename && !file_exists_p (object_filename))
    error ("Object '%s' not found.", object_filename);

  if (find_objfile_by_name (bundle_filename))
    error ("Bundle '%s' has already been loaded.", bundle_filename);

  wipe = set_current_language (source_filename);

  /* FIXME: Should use a cleanup to free cur if it's a newly allocated
     fixinfo and we bail before the end.  cf the documentation around
     the 'complete' field and free_half_finished_fixinfo().  */

  cur = get_fixinfo_for_new_request (source_filename);
  cur->bundle_filename = bundle_filename;
  cur->bundle_basename = getbasename (bundle_filename);
  cur->object_filename = object_filename;

  find_original_object_file_name (cur);

  tell_zerolink (cur);

  pre_load_and_check_file (cur);

  get_fixed_file (cur); 

  mark_previous_fixes_obsolete (cur);

  do_final_fix_fixups (cur);

  print_active_functions (cur);

  do_cleanups (wipe);
}

/* If the inferior process is a zerolinked executible, and the object
   file that we're about to replace hasn't yet been loaded in, we need
   to reference a symbol from the file and get ZL to map in the original
   objfile before we load the fixed version.  */

/* SPI for __zero_link_force_link_object_file() return value: */

enum ZLObjectFileResult {
  zlObjectFileUnknown	      = 0,
  zlObjectFileBeingLinked     = 1,
  zlObjectFileAlreadyLinked   = 2,
  zlObjectFileJustLinked      = 3,
};

static void
tell_zerolink (struct fixinfo *cur)
{
  static struct cached_value *cached_zl_force_link_object_file = NULL;
  struct value **obj_name_vec, **args, *val;
  const char *obj_name = cur->object_filename;
  int obj_name_len, i, retval;

  /* Is this source file already been fixed in the past?  */
  if (cur->fixed_object_files != NULL)
    return;

  /* Is the inferior using ZeroLink?  */
  if (!inferior_is_zerolinked_p ())
    return;

  if (obj_name == NULL)
    {
      warning ("Inferior is a ZeroLinked, but no .o file was provided.");
      return;
    }

  if (!lookup_minimal_symbol ("__zero_link_force_link_object_file", NULL, NULL))
    {
      warning ("Inferior is apparently a ZeroLink app, but "
               "__zero_link_force_link_object_file not found.");
      return;
    }

  if (cached_zl_force_link_object_file == NULL)
    cached_zl_force_link_object_file = create_cached_function
                  ("__zero_link_force_link_object_file", builtin_type_int);

  obj_name_len = strlen (obj_name);
  
  obj_name_vec = (struct value **) alloca 
                         (sizeof (struct value *) * (obj_name_len + 2));

  for (i = 0; i < obj_name_len + 1; i++)
    obj_name_vec[i] = value_from_longest (builtin_type_char, obj_name[i]);

  args = (struct value **) alloca (sizeof (struct value *) * 3);
  args[0] = value_array (0, obj_name_len, obj_name_vec);
  args[1] = value_from_longest (builtin_type_int, 0);

  val = call_function_by_hand_expecting_type
      (lookup_cached_function (cached_zl_force_link_object_file), 
       builtin_type_int, 1, args, 1);
  
  retval = value_as_long (val);

  if (fix_and_continue_debug_flag)
    switch (retval)
      {
        case zlObjectFileUnknown:
          printf_filtered ("DEBUG: zlObjectFileUnknown result from ZL.\n");
          break;
        case zlObjectFileBeingLinked:
          printf_filtered ("DEBUG: zlObjectFileBeingLinked result from ZL.\n");
          break;
        case zlObjectFileAlreadyLinked:
          printf_filtered ("DEBUG: zlObjectFileAlreadyLinked result from ZL.\n");
          break;
        case zlObjectFileJustLinked:
          printf_filtered ("DEBUG: zlObjectFileJustLinked result from ZL.\n");
          break;
        default:
          printf_filtered ("DEBUG: Got unknown result from ZeroLink!");
      }

  if (retval == zlObjectFileAlreadyLinked || retval == zlObjectFileJustLinked)
    return;
  else if (retval == zlObjectFileUnknown)
    warning ("ZeroLink says object file '%s' is unknown.", obj_name);
  else if (retval == zlObjectFileBeingLinked)
    warning ("ZeroLink says object file '%s' is mid-load.", obj_name);
  else
    warning ("Unrecognized result code from ZeroLink for obj file '%s'.", obj_name);
}

static int 
inferior_is_zerolinked_p (void)
{
  int is_zl_executable = 0;

  if (find_objfile_by_name (
    "/System/Library/PrivateFrameworks/ZeroLink.framework/Versions/A/ZeroLink"))
    {
      is_zl_executable = 1;
    }

  if (fix_and_continue_debug_flag && is_zl_executable)
    printf_filtered ("DEBUG: Inferior is a ZeroLink executable.\n");

  return (is_zl_executable);
}

/* Step through all previously fixed versions of this .o file and
   make sure their msymbols and symbols are marked obsolete.  */

static void
mark_previous_fixes_obsolete (struct fixinfo *cur)
{
  struct fixedobj *fo;
  struct minimal_symbol *msym;

  for (fo = cur->fixed_object_files; fo != NULL; fo = fo->next)
    {
      if (fo->objfile == NULL)
        {
          warning ("fixed object file entry for '%s' has a NULL objfile ptr!  "
                   "Will try continuing", fo->bundle_filename);
          continue;
        }

      /* Don't mark the file we just loaded as obsolete.
         This means that mark_previous_fixes_obsolete() must be run
         after get_fixed_file().  */

      if (fo == cur->most_recent_fix)
        continue;

      ALL_OBJFILE_MSYMBOLS (fo->objfile, msym)
        MSYMBOL_OBSOLETED (msym) = 1;

      int i, j;
      struct symbol *sym;
      struct symtab *st;
      struct partial_symtab *pst;

      ALL_OBJFILE_SYMTABS_INCL_OBSOLETED (fo->objfile, st)
        {
          if (st->primary == 1)
            for (i = 0; i < BLOCKVECTOR_NBLOCKS (BLOCKVECTOR (st)); i++)
              ALL_BLOCK_SYMBOLS (BLOCKVECTOR_BLOCK (BLOCKVECTOR (st), i), j, sym)
                SYMBOL_OBSOLETED (sym) = 1;
          SYMTAB_OBSOLETED (st) = 51;
        }

      ALL_OBJFILE_PSYMTABS (fo->objfile, pst)
        {
          PSYMTAB_OBSOLETED (pst) = 51;
        }
    }

    PSYMTAB_OBSOLETED (find_original_psymtab (cur)) = 51;
    SYMTAB_OBSOLETED (find_original_symtab (cur)) = 51;
}

/* Given a source filename, either find an existing fixinfo record
   detailing a previous fix, or create a new one and attach it to the
   chain. */

static struct fixinfo *
get_fixinfo_for_new_request (const char *source_filename)
{
  struct fixinfo *i, *prev = NULL;

  /* Let's scan over the list to make sure there aren't any half-allocated
     fixinfo structures.  FIXME, this is obviously a hack; see the 
     documentation for the fixinfo->complete variable at the top of
     the file about why I haven't solved this yet.  */

  i = fixinfo_chain;
  while (i != NULL)
    {
      if (i->complete == 0 && prev == NULL)
        {
          fixinfo_chain = i->next;
          free_half_finished_fixinfo (i);
          i = fixinfo_chain;
          continue;
        }
      if (i->complete == 0)
        {
          prev->next = i->next;
          free_half_finished_fixinfo (i);
          i = fixinfo_chain;
          continue;
        }
      prev = i;
      i = i->next;
    }

  for (i = fixinfo_chain; i != NULL; i = i->next)
    {
      if (!strcmp (i->src_filename, source_filename) && i->complete)
        return (i);
    }

  /* Either no match or no fixinfo entries */

  i = (struct fixinfo *) xmalloc (sizeof (struct fixinfo));
  memset (i, 0, sizeof (struct fixinfo));

  if (fixinfo_chain == NULL)
    fixinfo_chain = i;
  else
    i->next = fixinfo_chain;

  i->src_filename = source_filename;
  i->src_basename = getbasename (source_filename);
  i->complete = 0;

  return (i);
}

static void
free_half_finished_fixinfo (struct fixinfo *f)
{

  if (f->fixed_object_files != NULL)
    {
      warning ("free_half_finished_fixinfo: incomplete fixinfo was too complete");
      return;
    }

  if (f->src_filename != NULL)
    xfree ((char *) f->src_filename);
  if (f->bundle_filename != NULL)
    xfree ((char *) f->bundle_filename);
  if (f->object_filename != NULL)
    xfree ((char *) f->object_filename);
  if (f->active_functions != NULL)
    free_active_threads_struct (f->active_functions);
  if (f->original_objfile_filename != NULL)
    xfree ((char *) f->original_objfile_filename);

  xfree (f);
}

/* Given a mostly-populated CUR, load the named object file into the
   program via dyld and complete the fixinfo struct, CUR.  */

static void
get_fixed_file (struct fixinfo *cur)
{
  struct partial_symtab *ps = NULL;
  struct fixedobj *flp;

  struct fixedobj *fixedobj;
  struct objfile **objfile_list;

  int loaded_ok;

  /* Allocate a new fixedobj object for the .o file we're about to load,
     add it to the end of the CUR list of .o files. */

  fixedobj = xmalloc (sizeof (struct fixedobj));
  fixedobj->bundle_filename = cur->bundle_filename;
  fixedobj->firstdatum = NULL;
  fixedobj->lastdatum = NULL;
  fixedobj->obsoletedsym = NULL;
  fixedobj->objfile = NULL;
  fixedobj->next = NULL;
  
  /* FIXME: leak of fixedobj if load_fixed_objfile() errors out.  */
  /* FIXME: leak of objfile_list if load_fixed_objfile() errors out.  */

  /* Get a list of all objfiles gdb knows about, load the new .o file,
     figure out which one we just loaded. */

  objfile_list = build_list_of_current_objfiles ();

  loaded_ok = load_fixed_objfile (fixedobj->bundle_filename);

  fixedobj->objfile = 
                  find_newly_added_objfile (objfile_list, cur->bundle_filename);
  xfree (objfile_list);

  /* Even if the load_fixed_objfile() eventually failed, gdb may still believe
     a new solib was loaded successfully -- clear that out.  */
  if (loaded_ok != 1)
    {
#ifdef NM_NEXTSTEP
      if (fixedobj->objfile != NULL)
        remove_objfile_from_dyld_records (fixedobj->objfile);
#endif
      error ("NSLinkModule was not able to correctly load the Fix bundle, "
             "most likely due to unresolved external references.");
    }

  if (fixedobj->objfile == NULL)
    error ("Unable to load fixed object file.");

  /* Throw fixedobj on to the cur->fixed_object_files linked list.  */

  if (cur->fixed_object_files == NULL)
    cur->fixed_object_files = fixedobj;
  else
    {
      for (flp = cur->fixed_object_files; flp->next != NULL; flp = flp->next)
        ;
      flp->next = fixedobj;
    }
  cur->most_recent_fix = fixedobj;

  /* Should this psymtab expansion just be limited to the source file
     that we've just fixed?  It wouldn't catch changes in .h files, but
     then again I'm afraid we could have a lot of unnecessary psymtab
     expansion in an environment with a lot of header files.. */

  ALL_OBJFILE_PSYMTABS (fixedobj->objfile, ps)
    psymtab_to_symtab (ps);

  cur->complete = 1;
}

/* Get the final filename component of a pathname.
   Returns a pointer into the middle of NAME, or a pointer to NAME.
   So don't go around trying to deallocate the value returned here.  */
   
static const char *
getbasename (const char *name)
{
  const char *p = strrchr (name, '/');
  if (p)
    return (p + 1);
  else
    return name;
}

/* Returns 1 if the bundle loads correctly; 0 if it did not.
   If 0 is returned, the objfile linked list must be pruned of this
   aborted objfile load.  This cleanup is the responsibility of the caller.

  Do inferior function calls as if the inferior had done this:

  char *fn = "b2.o";
  NSObjectFileImage objfile_ref;
  NSObjectFileImageReturnCode retval1;
  NSModule retval2;

  retval1 = NSCreateObjectFileImageFromFile (fn, &objfile_ref);
  
  retval2 = NSLinkModule (objfile_ref, fn, NSLINKMODULE_OPTION_PRIVATE | NSLINKMODULE_OPTION_DONT_CALL_MOD_INIT_ROUTINES |NSLINKMODULE_OPTION_RETURN_ON_ERROR |NSLINKMODULE_OPTION_BINDNOW);

  Note that objfile_ref is first passed by reference, then by value, so
  we need to allocate space in the inferior for that ahead of time.

*/

static int
load_fixed_objfile (const char *name)
{
  struct value **libraryvec, *val, **args;
  int i, librarylen;
  struct value *objfile_image_ref_memory, *objfile_image_ref;
  struct value *ref_to_NSCreateObjectFileImageFromFile, *ref_to_NSLinkModule;
  NSObjectFileImageReturnCode retval;

  /* NSCreateObjectFileImageFromFile (NSObjectFileImage(3)) returns
     a pointer to an opaque structure via the 2nd argument you pass to it,
     which is a reference to a word of memory.  */
  objfile_image_ref_memory = 
           value_allocate_space_in_inferior (sizeof (NSObjectFileImage));

  ref_to_NSCreateObjectFileImageFromFile = find_function_in_inferior 
                        ("NSCreateObjectFileImageFromFile", builtin_type_int);

  ref_to_NSLinkModule = find_function_in_inferior 
                        ("NSLinkModule", builtin_type_int);

  librarylen = strlen (name);
  libraryvec = (struct value **) 
                    alloca (sizeof (struct value *) * (librarylen + 2));
  for (i = 0; i < librarylen + 1; i++)
    libraryvec [i] = value_from_longest (builtin_type_char, name[i]);

  /* Call to NSCreateObjectFileImageFromFile() */

  args = (struct value **) alloca (sizeof (struct value *) * 4);
  args[0] = value_array (0, librarylen, libraryvec);
  args[1] = objfile_image_ref_memory;
  args[2] = value_from_longest (builtin_type_int, 0);
  val = call_function_by_hand_expecting_type 
         (ref_to_NSCreateObjectFileImageFromFile, builtin_type_int, 3, args, 1);
  retval = value_as_long (val);
  if (retval != NSObjectFileImageSuccess)
    {
      error ("NSCreateObjectFileImageFromFile failed. "
             "This can happen if certain QuickDraw calls are happening in a "
             "run loop.  Stop your program with a normal breakpoint and "
             "re-try fix while stopped in your code.");
    }
  /* Call to NSLinkModule */

  objfile_image_ref = value_at (builtin_type_CORE_ADDR, 
                                value_as_long (objfile_image_ref_memory), NULL);
  args[0] = objfile_image_ref;
  args[1] = value_array (0, librarylen, libraryvec);
  args[2] = value_from_longest (builtin_type_int, 
                     NSLINKMODULE_OPTION_PRIVATE | 
                     NSLINKMODULE_OPTION_DONT_CALL_MOD_INIT_ROUTINES |
                     NSLINKMODULE_OPTION_RETURN_ON_ERROR |
                     NSLINKMODULE_OPTION_BINDNOW);
  args[3] = value_from_longest (builtin_type_int, 0);
  val = call_function_by_hand_expecting_type 
     (ref_to_NSLinkModule, builtin_type_int, 4, args, 1);

  retval = value_as_long (val);

  if (retval == 0)  /* NSLinkModule returns NULL on failed load.  */
    return 0;

  return 1;
}


/* Record the list of object files that gdb currently knows about.
   We'll then do some sort of operation that adds an object file to
   the list, and we want to know what got added.  Returns malloc()'ed
   memory that should be freed.  
   I'm assuming that an objfile structure will stay at the same address
   once it's allocated, but I'm pretty sure that's true...  */

static struct objfile **
build_list_of_current_objfiles (void)
{
  int size = 20;
  int j;
  struct objfile *i;
  struct objfile **list = (struct objfile **) 
                          xmalloc (sizeof (struct objfile *) * size);

  if (list == NULL) 
    error ("Ran out of memory while preparing to fix.");

  j = 0;
  ALL_OBJFILES (i)
    {
      list[j++] = i;
      if (j == size)
        {
          size = size * 2;
          list = (struct objfile **) xrealloc (list, sizeof (struct objfile *) * size);
          if (list == NULL)
            error ("Ran out of memory while preparing to fix.");
        }
    }
  list[j] = NULL;

  return list;
}

/* Given a list of object files that we know are old, and the name of a
   newly added object file, return a pointer to the struct objfile for
   that object file in the chain. */

static struct objfile *
find_newly_added_objfile (struct objfile **objlist, const char *objname)
{
  struct objfile *i;
  int j;

  ALL_OBJFILES (i)
    {
      if (!strcmp (i->name, objname))
        {
          j = 0;
          while (objlist[j] != NULL)
            {
              if (objlist[j] == i)
                break;
              j++;
            }
          if (objlist[j] == NULL)   /* not found in objlist */
            return (i);
        }
    }
  return NULL;
}


/* Step through a newly loaded object file's symbols looking for
   functions that need to be redirected and such.  
   FIXME: Do something with file-static indirect data in the new .o file
   to point to the original objfile.  Somehow.  */

static void 
do_final_fix_fixups (struct fixinfo *cur)
{
  struct objfile *newobj = cur->most_recent_fix->objfile;
  struct symtab *newsymtab = NULL;
  struct blockvector *newbv;
  struct block *newblock;
  struct objfile **objfiles_to_update;
  struct cleanup *cleanups;
  int i;
   
  objfiles_to_update = build_list_of_objfiles_to_update (cur);
  cleanups = make_cleanup (xfree, objfiles_to_update);

  for (i = 0; objfiles_to_update[i] != NULL; i++)
    {
      ALL_OBJFILE_SYMTABS_INCL_OBSOLETED (newobj, newsymtab)
        {

          /* All code-less symtabs will have links to a single codeful
             symtab's blockvector.  We only need to scan that blockvector
             once, so skip all the copy-cats. */
          if (newsymtab->primary != 1)
              continue;

          /* global symbols */
          newbv = BLOCKVECTOR (newsymtab);
          newblock = BLOCKVECTOR_BLOCK (newbv, GLOBAL_BLOCK);

          do_final_fix_fixups_global_syms (newblock, objfiles_to_update[i], cur);

          /* Static symbols (incl ObjC class functions) */
          newblock = BLOCKVECTOR_BLOCK (newbv, STATIC_BLOCK);

          do_final_fix_fixups_static_syms (newblock, objfiles_to_update[i], cur);
        }
    }

  redirect_file_statics (cur);

  do_cleanups (cleanups);
}

/* With the -findirect-data compiler flag, references to file static
   data (globals allocated in this compilation unit, actual static
   data) are made indirect (which is not normally necessary).  When
   the compilation unit is fixed, the target of these indirect
   pointers are redirected (by gdb) to the original static/global
   address locations.  */

static void
redirect_file_statics (struct fixinfo *cur)
{
  struct file_static_fixups *indirect_entries;
  struct cleanup *wipe;
  int indirect_entry_count;

  indirect_entry_count = find_and_parse_nonlazy_ptr_sect (cur, 
                                                       &indirect_entries);
  if (indirect_entries == NULL || indirect_entry_count == 0)
    return;
  
  wipe = make_cleanup (xfree, indirect_entries);

  find_new_static_symbols (cur, indirect_entries, indirect_entry_count);

  find_orig_static_symbols (cur, indirect_entries, indirect_entry_count);

  redirect_statics (indirect_entries, indirect_entry_count);
  
  do_cleanups (wipe);
}

static void
find_new_static_symbols (struct fixinfo *cur, 
                         struct file_static_fixups *indirect_entries,
                         int indirect_entry_count)
{
  struct symtab *symtab;
  struct block *b;
  struct symbol *sym;
  int i, j;

   for (j = 0; j < indirect_entry_count; j++)
     {
       CORE_ADDR addr = indirect_entries[j].value;
       int matched = 0;
 
       ALL_OBJFILE_SYMTABS (cur->most_recent_fix->objfile, symtab)
         {
           if (symtab->primary != 1)
             continue;
 
           b = BLOCKVECTOR_BLOCK (BLOCKVECTOR (symtab), STATIC_BLOCK);
           ALL_BLOCK_SYMBOLS (b, i, sym)
             {
               if (SYMBOL_CLASS (sym) == LOC_STATIC && 
                   SYMBOL_VALUE_ADDRESS (sym) == addr)
                 {
                   indirect_entries[j].new_sym = sym;
                   matched = 1;
                   break;
                 }
             }
           if (!matched)
             {
 
               b = BLOCKVECTOR_BLOCK (BLOCKVECTOR (symtab), GLOBAL_BLOCK);
               ALL_BLOCK_SYMBOLS (b, i, sym)
                 {
                   if (SYMBOL_CLASS (sym) == LOC_STATIC && 
                       SYMBOL_VALUE_ADDRESS (sym) == addr)
                    {
                       indirect_entries[j].new_sym = sym;
                       matched = 1;
                       break;
                     }
                 }
              }
 
           if (matched)
             {
               indirect_entries[j].new_msym =
                  lookup_minimal_symbol
                        (SYMBOL_LINKAGE_NAME (indirect_entries[j].new_sym),
                        NULL, cur->most_recent_fix->objfile);
             }
         }
    }
}

static void
find_orig_static_symbols (struct fixinfo *cur, 
                          struct file_static_fixups *indirect_entries,
                          int indirect_entry_count)
{
  int i;
  struct symbol *new_sym, *orig_sym;
  struct block *static_bl, *global_bl;
  struct objfile *original_objfile = find_original_object_file (cur);
  struct symtab *original_symtab = find_original_symtab (cur);

  static_bl = BLOCKVECTOR_BLOCK (BLOCKVECTOR (original_symtab), STATIC_BLOCK);
  global_bl = BLOCKVECTOR_BLOCK (BLOCKVECTOR (original_symtab), GLOBAL_BLOCK);

  for (i = 0; i < indirect_entry_count; i++)
    {
      new_sym = indirect_entries[i].new_sym;
      if (new_sym == NULL)
        continue;

      orig_sym = lookup_block_symbol (static_bl, SYMBOL_SOURCE_NAME (new_sym),
                                      SYMBOL_LINKAGE_NAME (new_sym), 
                                      SYMBOL_NAMESPACE (new_sym));
      if (orig_sym == NULL)
        orig_sym = lookup_block_symbol (global_bl, SYMBOL_SOURCE_NAME (new_sym),
                                        SYMBOL_LINKAGE_NAME (new_sym), 
                                        SYMBOL_NAMESPACE (new_sym));

      /* For C++ coalesced symbols, expand the scope of the search to
         other symtabs within this objfile.  */
      if (orig_sym == NULL)
        orig_sym = search_for_coalesced_symbol (original_objfile, new_sym);

      if (orig_sym)
        {
          indirect_entries[i].original_sym = orig_sym;
          indirect_entries[i].original_msym = 
               lookup_minimal_symbol (SYMBOL_LINKAGE_NAME (orig_sym), 
                                      NULL, original_objfile);
          if (indirect_entries[i].original_msym == NULL && fix_and_continue_debug_flag)
            {
              printf_unfiltered ("DEBUG: unable to find new msym for %s\n", SYMBOL_LINKAGE_NAME (orig_sym));
            }
        }
    }
}

/* This function does the actual overwriting of the indirect pointers
   for file statics in the module just loaded (just fixed).  It changes
   the pointers to point to the original file's version, obsoletes the
   fixed file's symbol/msymbol for that static, and unobsoletes the original
   file's symbol/msymbol.  */

static void 
redirect_statics (struct file_static_fixups *indirect_entries, 
                  int indirect_entry_count)
{
  int i;
  char buf[TARGET_ADDRESS_BYTES];

  for (i = 0; i < indirect_entry_count; i++)
    {
      if (fix_and_continue_debug_flag)
        {
          if (indirect_entries[i].addr == (CORE_ADDR) NULL)
            printf_filtered
               ("DEBUG: Static entry addr for file static #%d was zero.\n", i);
          if (indirect_entries[i].value == (CORE_ADDR) NULL)
            printf_filtered
                ("DEBUG: Destination addr for file static #%d was zero.\n", i);
          if (indirect_entries[i].new_sym == NULL)
            printf_filtered 
                   ("DEBUG: Could not find new symbol for static #%d\n", i);
          if (indirect_entries[i].new_msym == NULL)
            printf_filtered 
                 ("DEBUG: Could not find new msymbol for static #%d\n", i);
          if (indirect_entries[i].original_sym == NULL)
            printf_filtered 
                 ("DEBUG: Could not find original symbol for static #%d\n", i);
          if (indirect_entries[i].original_msym == NULL)
            printf_filtered 
                 ("DEBUG: Could not find original msymbol for static #%d\n", i);
        }

      if (indirect_entries[i].new_sym == NULL || 
          indirect_entries[i].original_sym == NULL ||
          indirect_entries[i].new_msym == NULL || 
          indirect_entries[i].original_msym == NULL)
        continue;
      if (indirect_entries[i].value == (CORE_ADDR) NULL || 
          indirect_entries[i].addr == (CORE_ADDR) NULL)
        continue;


      store_address (buf, TARGET_ADDRESS_BYTES, 
                     SYMBOL_VALUE_ADDRESS (indirect_entries[i].original_sym));
      write_memory (indirect_entries[i].addr, buf, TARGET_ADDRESS_BYTES);

      SYMBOL_OBSOLETED (indirect_entries[i].original_sym) = 0;
      MSYMBOL_OBSOLETED (indirect_entries[i].original_msym) = 0;
      SYMBOL_OBSOLETED (indirect_entries[i].new_sym) = 1;
      MSYMBOL_OBSOLETED (indirect_entries[i].new_msym) = 1;
      
      if (fix_and_continue_debug_flag)
        printf_filtered ("DEBUG: Redirected file static %s from 0x%s to 0x%s\n",
          SYMBOL_SOURCE_NAME (indirect_entries[i].original_sym),
          paddr_nz (SYMBOL_VALUE_ADDRESS (indirect_entries[i].new_sym)),
          paddr_nz (SYMBOL_VALUE_ADDRESS (indirect_entries[i].original_sym)));
    }
}


/* The indirect addresses are in a separate segment/section,
   (__DATA, __nl_symbol_ptr).  Find them, put them in an (xmalloced) 
   array of file_static_fixups, and return the number of them.  */

static int
find_and_parse_nonlazy_ptr_sect (struct fixinfo *cur, 
                                 struct file_static_fixups **indirect_entries)
{
  struct objfile *new_obj;
  struct obj_section *indirect_ptr_section, *j;
  CORE_ADDR indirect_ptr_section_start;
  bfd_size_type indirect_ptr_section_size;
  int nl_symbol_ptr_count = 0;
  int actual_entry_count;
  char *buf;
  struct cleanup *wipe;
  int i;
  
  *indirect_entries = NULL;
  indirect_ptr_section = NULL;
  new_obj = cur->most_recent_fix->objfile;


  ALL_OBJFILE_OSECTIONS (new_obj, j)
    if (!strcmp ("LC_SEGMENT.__DATA.__nl_symbol_ptr", 
                bfd_section_name (new_obj->obfd, j->the_bfd_section)))
      {
        indirect_ptr_section = j;
        break;
      }

  if (indirect_ptr_section == NULL)
    return 0;

  indirect_ptr_section_start = indirect_ptr_section->addr;

  indirect_ptr_section_size = 
           indirect_ptr_section->endaddr - indirect_ptr_section->addr;

  if (indirect_ptr_section_size == 0)
    return 0;

  if (indirect_ptr_section_size % TARGET_ADDRESS_BYTES != 0)
    error ("Incorrect __DATA, __nl_symbol_ptr section size!");

  nl_symbol_ptr_count = indirect_ptr_section_size / TARGET_ADDRESS_BYTES;
  *indirect_entries = (struct file_static_fixups *) xmalloc 
                   (sizeof (struct file_static_fixups) * nl_symbol_ptr_count);

  buf = xmalloc (nl_symbol_ptr_count * TARGET_ADDRESS_BYTES);
  wipe = make_cleanup (xfree, buf);

  /* The following code to read an array of ints from the target and convert
     them to host order is from symfile.c:read_target_long_array.  */

  read_memory (indirect_ptr_section_start, buf,
               nl_symbol_ptr_count * TARGET_ADDRESS_BYTES);

  /* Some of these entries will point to objects outside the current object
     file, in which case we're not interested in them.  */

  actual_entry_count = 0;
  for (i = 0; i < nl_symbol_ptr_count; i++)
    {
      CORE_ADDR destination_address;
      destination_address = extract_unsigned_integer
                       (buf + i * TARGET_ADDRESS_BYTES, TARGET_ADDRESS_BYTES);

      if (destination_address == 0 ||
          find_pc_section (destination_address)->objfile != 
                                    cur->most_recent_fix->objfile)
        continue;

      (*indirect_entries)[actual_entry_count].addr = 
                         indirect_ptr_section_start + i * TARGET_ADDRESS_BYTES;
      (*indirect_entries)[actual_entry_count].value = destination_address;
      (*indirect_entries)[actual_entry_count].new_sym = NULL;
      (*indirect_entries)[actual_entry_count].new_msym = NULL;
      (*indirect_entries)[actual_entry_count].original_sym = NULL;
      (*indirect_entries)[actual_entry_count].original_msym = NULL;
      actual_entry_count++;
    }

  do_cleanups (wipe);

  return (actual_entry_count);
}


/* Build up a list of object files we need to scan to redirect
   old functions to the new versions.  */

static struct objfile **
build_list_of_objfiles_to_update (struct fixinfo *cur)
{
  struct fixedobj *i;
  struct objfile **old_objfiles;
  int count, j;

  count = 2;  /* 1 for original_objfile, 1 for NULL */

  for (i = cur->fixed_object_files; i != NULL; i = i->next)
    {
      if (i == cur->most_recent_fix)
        continue;
      count++;
    }

  old_objfiles = (struct objfile **) 
                   xmalloc (sizeof (struct objfile *) * count);

  old_objfiles[0] = find_original_object_file (cur);
  j = 1;

  for (i = cur->fixed_object_files; i != NULL; i = i->next)
    {
      if (i == cur->most_recent_fix)
        continue;
      old_objfiles[j++] = i->objfile;
    }
  old_objfiles[j] = NULL;

  return (old_objfiles);
}


/* Look for function names in the global scope of a just-loaded object file.
   When found, try to find that same function name in the old object file,
   and stomp on that function's prologue if found.  */

void
do_final_fix_fixups_global_syms (struct block *newglobals, 
                                 struct objfile *oldobj, 
                                 struct fixinfo *curfixinfo)
{
  struct symtab *oldsymtab;
  struct blockvector *oldbv;
  struct block *oldblock;
  struct symbol *oldsym = NULL;
  int j;
  struct symbol *cursym, *newsym;         

  ALL_BLOCK_SYMBOLS (newglobals, j, cursym)
    {
      newsym = lookup_block_symbol (newglobals, SYMBOL_SOURCE_NAME (cursym), 
                                 SYMBOL_LINKAGE_NAME (cursym), VAR_NAMESPACE);
      /* Ignore type definitions. */
      if (!newsym || SYMBOL_CLASS (newsym) == LOC_TYPEDEF)
        continue;

      if (!oldobj)
        continue;

      ALL_OBJFILE_SYMTABS_INCL_OBSOLETED (oldobj, oldsymtab)
        {
          /* All code-less symtabs will have links to a single codeful
             symtab's blockvector.  We only need to scan that blockvector
             once, so skip all the copy-cats. */
          if (oldsymtab->primary != 1)
              continue;

          oldbv = BLOCKVECTOR (oldsymtab);
          oldblock = BLOCKVECTOR_BLOCK (oldbv, GLOBAL_BLOCK);
          if (oldblock != newglobals)
            {
              oldsym = lookup_block_symbol (oldblock, 
                             SYMBOL_SOURCE_NAME (cursym), 
                             SYMBOL_LINKAGE_NAME (cursym), VAR_NAMESPACE);
              if (oldsym)
                break;
            }
        }
        if (!oldsym)
          continue;

        /* Fixup a function. */
        if (TYPE_CODE (SYMBOL_TYPE (newsym)) == TYPE_CODE_FUNC)
          {
          if (fix_and_continue_debug_flag)
            printf_filtered ("DEBUG: fixed up global %s "
                         "(newaddr 0x%s, oldaddr 0x%s)\n", 
                         SYMBOL_NAME (newsym), 
                         paddr_nz (BLOCK_START (SYMBOL_BLOCK_VALUE (newsym))), 
                         paddr_nz (BLOCK_START (SYMBOL_BLOCK_VALUE (oldsym))));

          redirect_old_function (curfixinfo, newsym, oldsym,
                                  in_active_func (SYMBOL_LINKAGE_NAME (cursym), 
                                              curfixinfo->active_functions));
          }

    }
}

static void
do_final_fix_fixups_static_syms (struct block *newstatics, 
                                 struct objfile *oldobj, 
                                 struct fixinfo *curfixinfo)
{
  struct symtab *oldsymtab;
  struct blockvector *oldbv;
  struct block *oldblock;
  struct symbol *oldsym = NULL;
  int j;
  struct symbol *cursym, *newsym;         
  struct objfile *original_objfile = find_original_object_file (curfixinfo);

  ALL_BLOCK_SYMBOLS (newstatics, j, cursym)
    {
      newsym = lookup_block_symbol (newstatics, SYMBOL_SOURCE_NAME (cursym),
                                 SYMBOL_LINKAGE_NAME (cursym), VAR_NAMESPACE);
      /* Ignore type definitions. */
      if (!newsym || SYMBOL_CLASS (newsym) == LOC_TYPEDEF)
        continue;

      /* FIXME - skip over non-function syms */
      if (TYPE_CODE (SYMBOL_TYPE (newsym)) != TYPE_CODE_FUNC)
        continue;

      if (!oldobj)
        continue;

      ALL_OBJFILE_SYMTABS_INCL_OBSOLETED (oldobj, oldsymtab)
        {
          /* All code-less symtabs will have links to a single codeful
             symtab's blockvector.  We only need to scan that blockvector
             once, so skip all the copy-cats. */
          if (oldsymtab->primary != 1)
              continue;

          oldbv = BLOCKVECTOR (oldsymtab);
          oldblock = BLOCKVECTOR_BLOCK (oldbv, STATIC_BLOCK);
          if (oldblock != newstatics)
            {
              oldsym = lookup_block_symbol (oldblock, 
                            SYMBOL_SOURCE_NAME (cursym), 
                            SYMBOL_LINKAGE_NAME (cursym), VAR_NAMESPACE);
              if (oldsym)
                break;
            }
        }

        /* Maybe this static is a C++ coalesced symbol that resides in some
           arbitrary symtab.  Try expanding the search scope a bit.  */
        if (!oldsym)
          {
            oldsym = search_for_coalesced_symbol 
                             (original_objfile, newsym);
            if (oldsym == newsym)
              oldsym = NULL;
          }

        if (!oldsym)
          continue;

        /* Fixup a function. */
        if (TYPE_CODE (SYMBOL_TYPE (newsym)) == TYPE_CODE_FUNC)
          {
            if (fix_and_continue_debug_flag)
              printf_filtered ("DEBUG: fixed up static %s "
                         "(newaddr 0x%s, oldaddr 0x%s)\n", 
                         SYMBOL_NAME (newsym), 
                         paddr_nz (BLOCK_START (SYMBOL_BLOCK_VALUE (newsym))), 
                         paddr_nz (BLOCK_START (SYMBOL_BLOCK_VALUE (oldsym))));

            redirect_old_function (curfixinfo, newsym, oldsym,
                         in_active_func (SYMBOL_LINKAGE_NAME (cursym), 
                                        curfixinfo->active_functions));
          }
    }
}

/* The instructions to put a 32 bit address into a register will
   sign extend the value of the lower 16 bits.  You put the higher
   16 bits into the register with an 'addis' instructions so you
   need to add 1 to the higher 16 bits to arrive at the correct
   value.  This corresponds to the "hi16()" and "lo16()" address
   transforms you see in assembly output.   */

static uint16_t 
encode_lo16 (CORE_ADDR addr) 
{
  return addr & 0xffff;
}

static uint16_t 
encode_hi16 (CORE_ADDR addr) 
{
  uint16_t i;
  i = addr >> 16;

  /* is bit 15 set? */
  if (addr & 0x8000)
    i++;

  return i;
}

static CORE_ADDR 
decode_hi16_lo16 (uint16_t hi16, uint16_t lo16)
{
  CORE_ADDR addr = 0;

  addr = lo16;

  /* If the high bit of lo16 was set, we need to decrement hi16 by one. */
  if (lo16 & 0x8000)
    hi16--;

  addr = addr | ((CORE_ADDR) hi16 << 16);

  return addr;
}

/* FIXME: This is a copy of objfiles.c:do_free_objfile_cleanup which is static.
   Maybe it shouldn't be. */

static void
free_objfile_cleanup (void *obj)
{
  free_objfile (obj);
}

/* Before we load an objfile via dyld, load it into gdb and check that
   it doesn't violate any of the easy-to-check restrictions.  We fill in
   a good bit of the fixinfo structure as we do our job. */

static void
pre_load_and_check_file (struct fixinfo *cur)
{
  struct section_addr_info section_addrs;
  bfd *object_bfd;
  struct objfile *new_objfile;
  struct cleanup *cleanups;

  /* FIXME: I'm not too confident of the way I'm calling bfd_open here -
     I should really look more into whether I need to provide some kind of
     load address... */

  memset (&section_addrs, 0, sizeof (struct section_addr_info));
  object_bfd = symfile_bfd_open_safe (cur->bundle_filename, 0);
  new_objfile = symbol_file_add_bfd_safe (object_bfd, 0, &section_addrs, 
                                          0, 0, OBJF_SYM_ALL, (CORE_ADDR) NULL, 
                                          NULL);

  cleanups = make_cleanup (free_objfile_cleanup, new_objfile);

  force_psymtab_expansion (new_objfile, cur->src_filename, cur->src_basename);

  cur->active_functions = create_current_threads_list (cur->src_filename);

  do_pre_load_checks (cur, new_objfile);

  do_cleanups (cleanups);   /* De-allocate the objfile we pre-loaded */
}


/* Iterate through the threads linked list and all the functions in
   each thread's stack, to make an exhaustive list of all currently-executing
   functions which are being replaced by the fix command.  */

static struct active_threads *
create_current_threads_list (const char *source_filename)
{
  struct thread_info *tp;
  struct active_threads *i, *head;
  struct active_func *k;
  struct cleanup *inferior_ptid;
  struct ui_out *null_uiout = NULL;
  char buf[80];
  enum gdb_rc rc;

  i = head = NULL;

  /* FIXME: gdb_thread_select has gained a "print" parameter, so this
     UI redirection is superfluous.  molenda/2003-04-23  */

  inferior_ptid = save_inferior_ptid ();
  null_uiout = cli_out_new (gdb_null);
  if (null_uiout == NULL)
    {
      error ("Unable to open null uiout in fix-and-continue.c");
    }
  make_cleanup_ui_out_delete (null_uiout);


  /* FIXME: I should use the iterate_over_threads () call.  */

  for (tp = thread_list; tp; tp = tp->next)
    {
      snprintf (buf, 79, "%d", tp->num);
      rc = gdb_thread_select (null_uiout, buf, 0); 

      if (((int) rc < 0 && (enum return_reason) rc == RETURN_ERROR) ||
          ((int) rc >= 0 && rc == GDB_RC_FAIL))
        {
          /* Thread's dead, Jed.  Silently continue on our way.  */
          continue;
        }

      k = create_current_active_funcs_list (source_filename);

      /* Any functions in this thread being replaced? */
      if (k != NULL)
        {
          if (head == NULL)
            {
              i = head = (struct active_threads *) xmalloc 
                                              (sizeof (struct active_threads));
            }
           else
             {
               i->next = (struct active_threads *) xmalloc 
                                              (sizeof (struct active_threads));
               i = i->next;
             }
          i->num = tp->num;
          i->active_func_chain = k;
          i->pc = read_pc ();
          i->next = NULL;
       }
    }

  do_cleanups (inferior_ptid);
  return (head);
}

/* Check out the object file for really obvious violations, like adding
   a parameter to a function that is currently on the stack.
   When this is called, the following things should already have been done:

     cur->original_objfile_filename and cur->canonical_source_filename are 
     initialized

     cur->active_functions is initialized for all threads

     the psymtab in the original and new objfiles have been expanded

      source_filename and bundle_filename are correct

   Most notably, we don't make any assumption that cur->fixed_object_files
   has anything in it yet (these checks are done before the object
   file is actually loaded into memory via dyld.)  */

   /* FIXME: Some of the things I need to check here:

        Are any functions that will be rewritten too small to have the
        F&C trampoline written?

        Is the PC in the area where I need to write a trampoline?

        Have the number of arguments of any functions on the stack changed?
        Were any of the types of those arguments changed?

        Have the global/statics been changed?  The statics/globals in the
        old original file should be identical to this one.

        Have any types been changed?  Things added to a struct, a struct
        turning into an int, etc.

     Maybe I can do a few others, but that's about what the wdb impl does.  */

static void
do_pre_load_checks (struct fixinfo *cur, struct objfile *new_objfile)
{
  if (cur->original_objfile_filename == NULL || 
      cur->canonical_source_filename == NULL)
    {
      internal_error (__FILE__, __LINE__, "do_pre_load_checks: "
                      "Original objfile or canonical source filename not set");
    }
  if (cur->src_filename == NULL || cur->bundle_filename == NULL)
    internal_error (__FILE__, __LINE__,
       "do_pre_load_checks: src_filename or bundle_filename not set");

  /* FIXME: We're going to error() out of these funcs if there is a problem;
     we need a cleanup (probably at the caller of this func) to clean up the
     pre-loaded objfile.  */

  check_restrictions_globals (cur, new_objfile);
  check_restrictions_statics (cur, new_objfile);
  check_restrictions_locals (cur, new_objfile);
  check_restriction_cxx_zerolink (new_objfile);
}


static void
check_restrictions_globals (struct fixinfo *cur, struct objfile *newobj)
{
  struct objfile *oldobj = find_original_object_file (cur);
  struct block *oldblock, *newblock;
  struct symtab *oldsymtab, *newsymtab;
  struct symbol *oldsym, *newsym, *sym;
  const char *sym_source_name, *sym_linkage_name;
  char *old_type, *new_type;
  struct cleanup *wipe;

  ALL_OBJFILE_SYMTABS_INCL_OBSOLETED (newobj, newsymtab)
    {
      if (newsymtab->primary != 1)
        continue;

      int i;
      newblock = BLOCKVECTOR_BLOCK (BLOCKVECTOR (newsymtab), GLOBAL_BLOCK);
      ALL_BLOCK_SYMBOLS (newblock, i, sym)
        {
          sym_source_name = SYMBOL_SOURCE_NAME (sym);
          sym_linkage_name = SYMBOL_LINKAGE_NAME (sym);
          newsym = lookup_block_symbol (newblock, sym_source_name, 
                                        sym_linkage_name, VAR_NAMESPACE);
          oldsym = NULL;
          if (newsym && (SYMBOL_CLASS (newsym) != LOC_TYPEDEF))
            {
              ALL_OBJFILE_SYMTABS_INCL_OBSOLETED (oldobj, oldsymtab)
                {
                   oldblock = BLOCKVECTOR_BLOCK 
                                      (BLOCKVECTOR (oldsymtab), GLOBAL_BLOCK);
                   oldsym = lookup_block_symbol (oldblock, sym_source_name, 
                                             sym_linkage_name, VAR_NAMESPACE);
                   if (oldsym)
                     break;
                }
            }

          /* FIXME: Should we bail if new globals are added?  */
          if (!oldsym)
            continue;

          /* Functions have class LOC_BLOCK.  */
          if (TYPE_CODE (SYMBOL_TYPE (oldsym)) == TYPE_CODE_FUNC && 
              TYPE_CODE (SYMBOL_TYPE (newsym)) != TYPE_CODE_FUNC)
            error ("Changing function '%s' to a variable is not supported.",
                   SYMBOL_SOURCE_NAME (oldsym));

          if (TYPE_CODE (SYMBOL_TYPE (oldsym)) != TYPE_CODE_FUNC && 
              TYPE_CODE (SYMBOL_TYPE (newsym)) == TYPE_CODE_FUNC)
            error ("Changing variable '%s' to a function is not supported.",
                   SYMBOL_SOURCE_NAME (oldsym));

          if (TYPE_CODE (SYMBOL_TYPE (oldsym)) == TYPE_CODE_FUNC && 
              TYPE_CODE (SYMBOL_TYPE (newsym)) == TYPE_CODE_FUNC)
            continue;

          old_type = type_sprint (SYMBOL_TYPE (oldsym), NULL, 0);
          wipe = make_cleanup (xfree, old_type);
          new_type = type_sprint (SYMBOL_TYPE (newsym), NULL, 0);
          make_cleanup (xfree, new_type);
          if (strcmp (old_type, new_type) != 0)
            error ("Changing the type of global variable '%s'"
                 " from '%s' to '%s' is not supported.",     
                 SYMBOL_SOURCE_NAME (oldsym), old_type, new_type);

          do_cleanups (wipe);

        }
    }
}

static void
check_restrictions_statics (struct fixinfo *cur, struct objfile *newobj)
{
  struct block *newblock;
  struct symtab *newsymtab;
  struct symbol *oldsym, *newsym, *sym;
  const char *sym_source_name, *sym_linkage_name;  
  struct objfile *original_objfile = find_original_object_file (cur);

  ALL_OBJFILE_SYMTABS_INCL_OBSOLETED (newobj, newsymtab)
    {
      if (newsymtab->primary != 1)
        continue;

      int i;
      newblock = BLOCKVECTOR_BLOCK (BLOCKVECTOR (newsymtab), STATIC_BLOCK);
      ALL_BLOCK_SYMBOLS (newblock, i, sym)
        {
          /* Notably: Skip STRUCT_NAMESPACE until I can think of checks for
             it.  */
          if (SYMBOL_NAMESPACE (sym) != VAR_NAMESPACE &&
              SYMBOL_NAMESPACE (sym) != METHODS_NAMESPACE)
            continue;

          sym_source_name = SYMBOL_SOURCE_NAME (sym);
          sym_linkage_name = SYMBOL_LINKAGE_NAME (sym);
          newsym = lookup_block_symbol (newblock, sym_source_name,      
                                      sym_linkage_name, SYMBOL_NAMESPACE (sym));


          /* This should be impossible. */
          /* Actually, it can happen.  If you're fixing a C++ program
             but some of the files don't end in ".cp" or ".cxx" or ".C",
             gdb's mangling will be disabled.  The mangled sym name is passed
             to lookup_symbol() and it won't match anything.
             A reasonable workaround at this point would be to try setting
             the current language to language_cplus and re-executing this
             function... */

          /* FIXME: This error message is a hack.  */
          if (newsym == NULL)
            error ("No symbol found for '%s'.  "
                   "Could this be a C++ application whose source filenames end in '.c'?", 
                    sym_source_name);

          if (SYMBOL_CLASS (newsym) == LOC_CONST)
            continue;
          if (TYPE_CODE (SYMBOL_TYPE (newsym)) == TYPE_CODE_FUNC)
            continue;

          /* For now, ignore all of the OBJC internal labels
             (_OBJC_CLASS, _OBJC_CLASS_METHODS, _OBJC_CLASS_NAME,
              _OBJC_CLASS_REFERENCES, _OBJC_INSTANCE_METHODS,
              _OBJC_INSTANCE_VARIABLES, _OBJC_METACLASS, _OBJC_METH_VAR_NAME,
              _OBJC_METH_VAR_TYPE, _OBJC_SELECTOR_REFERENCES, et al)
	     These may prove to be useful to check later on, but
	     I haven't thought this through yet, and I suspect only 
             _OBJC_INSTANCE_METHODS and _OBJC_INSTANCE_VARIABLES will 
             be of use.  */
          if (strncmp (sym_linkage_name, "_OBJC_", 6) == 0)
            continue;

          oldsym = lookup_symbol (SYMBOL_LINKAGE_NAME (newsym), NULL,
                                  SYMBOL_NAMESPACE (newsym), NULL, NULL);

          /* oldsym == newsym, so we didn't find the symbol in the symtabs.
             Try a bit more searching before we assume it's a new symbol.  
             This can easily happen in C++ where the symbol may be a coalesced
             sym in a symtab that hasn't been expanded from a psymtab yet.  */
          if (oldsym == newsym && 
              (SYMBOL_CLASS (newsym) == LOC_STATIC ||
               SYMBOL_CLASS (newsym) == LOC_INDIRECT ||
               SYMBOL_CLASS (newsym) == LOC_THREAD_LOCAL_STATIC))
            {
              oldsym = search_for_coalesced_symbol 
                               (original_objfile, newsym);
                  /* We didn't find a matching minsym in the objfile
                     (app, library, etc.) that we're fixing.  This should
                     be a reliable indication that a new static is being added
                     by the user.  Which we'll handle by ignoring for now.   */
              if (oldsym == newsym || oldsym == NULL)
                continue;
            }

          /* A new symbol if I'm not mistaken..  Let it pass.  */
          if (oldsym == NULL)
            continue;

          if (SYMBOL_CLASS (oldsym) != LOC_CONST &&
              SYMBOL_CLASS (oldsym) != LOC_TYPEDEF)
            {
              char *old_type, *new_type;
              struct cleanup *wipe;

              /* Hacky: In some programs the original static symbol type
                 might not have resolved correctly when the original objfile
                 was read in.  So in that case, we'll give the user the benefit
                 of the doubt and just skip the type change checks.  */
              if (TYPE_CODE (SYMBOL_TYPE (oldsym)) == TYPE_CODE_ERROR ||
                  TYPE_CODE (SYMBOL_TYPE (oldsym)) == TYPE_CODE_UNDEF)
                {
                  warning ("Type code for '%s' unresolvable, "
                           "skipping type change checks.", 
                              SYMBOL_SOURCE_NAME (oldsym));
                  continue;
                }
              if (TYPE_CODE (SYMBOL_TYPE (newsym)) == TYPE_CODE_ERROR ||
                  TYPE_CODE (SYMBOL_TYPE (newsym)) == TYPE_CODE_UNDEF)
                {
                  warning ("Type code for '%s' unresolvable, "
                           "skipping type change checks.", 
                              SYMBOL_SOURCE_NAME (newsym));
                  continue;
                }

              old_type = type_sprint (SYMBOL_TYPE (oldsym), NULL, 0);
              wipe = make_cleanup (xfree, old_type);
              new_type = type_sprint (SYMBOL_TYPE (newsym), NULL, 0);
              make_cleanup (xfree, new_type);
              if (strcmp (old_type, new_type) != 0)
                error ("Changing the type of file static variable '%s'"
                     " from '%s' to '%s' is not supported.", 
                     SYMBOL_SOURCE_NAME (oldsym), old_type, new_type);

              do_cleanups (wipe);
            }
        }
    }
}

static void
check_restrictions_locals (struct fixinfo *cur, struct objfile *newobj)
{
  struct objfile *oldobj = find_original_object_file (cur);
  struct block *oldblock, *newblock;
  struct symtab *oldsymtab, *newsymtab;
  struct blockvector *oldbv, *newbv;
  struct symbol *oldsym;
  const char *funcname;
  int active, i, j;
  int foundmatch;

  ALL_OBJFILE_SYMTABS_INCL_OBSOLETED (newobj, newsymtab)
    {
      if (newsymtab->primary != 1)
        continue;

      newbv = BLOCKVECTOR (newsymtab);
      for (i = FIRST_LOCAL_BLOCK; i < BLOCKVECTOR_NBLOCKS (newbv); i++)
        {
          newblock = BLOCKVECTOR_BLOCK (newbv, i);
          if (BLOCK_FUNCTION (newblock) == NULL)
            continue;
          funcname = SYMBOL_LINKAGE_NAME (BLOCK_FUNCTION (newblock));
          if (funcname == NULL)
            continue;

          active = in_active_func (funcname, cur->active_functions);

          ALL_OBJFILE_SYMTABS_INCL_OBSOLETED (oldobj, oldsymtab)
            {
              if (oldsymtab->primary != 1)
                continue;

              oldbv = BLOCKVECTOR (oldsymtab);
              foundmatch = 0;
              for (j = FIRST_LOCAL_BLOCK; j < BLOCKVECTOR_NBLOCKS (oldbv); j++)
                {
                  oldblock = BLOCKVECTOR_BLOCK (oldbv, j);
                  if (BLOCK_FUNCTION (oldblock) != NULL &&
                      SYMBOL_MATCHES_NAME (BLOCK_FUNCTION (oldblock), funcname))
                    {
                      check_restrictions_function (funcname, active,
                                                   oldblock, newblock);
                      foundmatch = 1;
                    }
                }
              /* This picks up the case where the function was coalesced into
                 another symtab within the same objfile ("C++").  */
              if (foundmatch == 0)
                {
                  oldsym = search_for_coalesced_symbol (oldobj, 
                                                    BLOCK_FUNCTION (newblock));
                  if (oldsym) 
                    {
                      oldblock = SYMBOL_BLOCK_VALUE (oldsym);
                      if (oldblock != newblock)
                        check_restrictions_function (funcname, active,
                                                     oldblock, newblock);
                    }
                }
            }
        }
    }
}

static void
check_restrictions_function (const char *funcname, int active, 
                             struct block *oldblock, struct block *newblock)
{
  int newfunc_args, oldfunc_args;
  int newfunc_locals, oldfunc_locals;
  int i, j;
  char *old_type_name, *new_type_name;
  struct symbol *oldsym, *newsym;
  struct cleanup *wipe;

  wipe = make_cleanup (null_cleanup, NULL);

  /* NB: The way I use step through the newblock and oldblock assume that
     the block is not sorted and is not a hashtable.  I believe this is
     currently an accurate assertion for function blocks.  */
  if (BLOCK_SHOULD_SORT (oldblock) || BLOCK_SHOULD_SORT (newblock) ||
      BLOCK_HASHTABLE (oldblock) != 0 || BLOCK_HASHTABLE (newblock) != 0)
    {
      internal_error (__FILE__, __LINE__,
      "check_restrictions_function: Got a block with a hash table or sortable.");
    }

  /* Check to see that the function return type matches.  */

  old_type_name = type_sprint (SYMBOL_TYPE (BLOCK_FUNCTION (oldblock)), NULL, 0);
  make_cleanup (xfree, old_type_name);
  new_type_name = type_sprint (SYMBOL_TYPE (BLOCK_FUNCTION (newblock)), NULL, 0);
  make_cleanup (xfree, new_type_name);

  if (strcmp (old_type_name, new_type_name) != 0)
    error ("Function '%s' was changed from returning '%s' to '%s', which is "
           "not supported.", funcname, old_type_name, new_type_name);


  /* Count # of args, locals in old and new blocks.  */
 
  oldfunc_args = oldfunc_locals = 0;
  ALL_BLOCK_SYMBOLS (oldblock, i, oldsym)
    {
      if (sym_is_argument (oldsym))
        oldfunc_args++;
      else if (sym_is_local (oldsym))
        oldfunc_locals++;
    }
 
  newfunc_args = newfunc_locals = 0;
  ALL_BLOCK_SYMBOLS (newblock, i, newsym)
    {
      if (sym_is_argument (newsym))
        newfunc_args++;
      else if (sym_is_local (newsym))
        newfunc_locals++;
    }

  if (oldfunc_args != newfunc_args)
    error ("Changing number of arguments from %d to %d for function '%s' is "
           "not supported.", oldfunc_args, newfunc_args, funcname);

  if (active && oldfunc_locals != newfunc_locals)
    error ("Changing number of local variables from %d to %d for function '%s'"
           " while active on the stack is not supported.", 
           oldfunc_locals, newfunc_locals, funcname);


  /* Check that arguments have matching types.  */

  /* In the following loop, we can access the symbols in the block
     structure directly, but if the block structure is ever replaced
     with the proposed 'dictionary' structure, we'll need to maintain
     concurrent accessor iterators to step through both the old
     and new blocks in tandem.  So I approximate that behavior with
     the two integers i and j.  molenda 2003-04-16.  */

  ALL_BLOCK_SYMBOLS (oldblock, i, oldsym)
    {
      j = i;

      /* oldblock has more syms than newblock, but the current sym isn't
         a local or an argument so I don't care.  */
      if (j >= BLOCK_NSYMS (newblock))
        continue;

      newsym = BLOCK_SYM (newblock, j);

      /* FIXME:  Still need to think through the "type equivalency" checks
         here.  Check by type, or by memory size?  Not sure yet.  */

      old_type_name = type_sprint (SYMBOL_TYPE (oldsym), NULL, 0);
      make_cleanup (xfree, old_type_name);
      new_type_name = type_sprint (SYMBOL_TYPE (newsym), NULL, 0);
      make_cleanup (xfree, new_type_name);

      if (sym_is_argument (oldsym) &&
          strcmp (old_type_name, new_type_name) != 0)
        {
          error ("In function '%s', argument '%s' changed from "
                 "type '%s' to type '%s', which is not supported.",
                 funcname, SYMBOL_SOURCE_NAME (oldsym), 
                 old_type_name, new_type_name);
        }
    }
  do_cleanups (wipe);
}

/* C++ programs must use ZeroLink, which implies using a shared libstdc++.
   The static libstdc++ private extern functions cannot be found by dyld
   after the program is linked together in a traditional link, so the fixed
   bundle cannot bind to them.  ZeroLink has a shared libstdc++ to deal with
   these very issues.  */

static void
check_restriction_cxx_zerolink (struct objfile *obj)
{
  struct symtab *s;
  
  if (inferior_is_zerolinked_p ())
    return;
  
  ALL_OBJFILE_SYMTABS (obj, s)
    if (s->primary && 
        (s->language == language_cplus || s->language == language_objcplus))
      error ("Target is a C++ program that has not using ZeroLink.  "
             "This is not supported.  To use Fix and Continue on a C++ program, "
             "enable ZeroLink.");
}

static int
sym_is_argument (struct symbol *s)
{
  return (SYMBOL_CLASS (s) == LOC_ARG ||
          SYMBOL_CLASS (s) == LOC_REF_ARG ||
          SYMBOL_CLASS (s) == LOC_REGPARM ||
          SYMBOL_CLASS (s) == LOC_REGPARM_ADDR ||
          SYMBOL_CLASS (s) == LOC_BASEREG_ARG);
}

static int
sym_is_local (struct symbol *s)
{
  return (SYMBOL_CLASS (s) == LOC_LOCAL ||
          SYMBOL_CLASS (s) == LOC_REGISTER ||
          SYMBOL_CLASS (s) == LOC_BASEREG);
}

/* Expand the partial symtabs for the named source file in the given
   objfile.  If an alternate source filename is provided, that one is
   searched for as well. */

static void
force_psymtab_expansion (struct objfile *obj, const char *source_fn, 
                         const char *alt_source_fn)
{
  struct partial_symtab *ps;

  /* Iterate over the objfile, expanding anything that looks like it might
     be the file we're interested in.  Expand anything that looks like a
     match--expanding too many isn't a travesty, but expanding none would
     be Bad.  */

  ALL_OBJFILE_PSYMTABS_INCL_OBSOLETED (obj, ps)
    if (!strcmp (source_fn, ps->filename))
      psymtab_to_symtab (ps);
    else if (ps->fullname != NULL && !strcmp (source_fn, ps->fullname))
      psymtab_to_symtab (ps);
    else if (alt_source_fn != NULL && !strcmp (alt_source_fn, ps->filename))
      psymtab_to_symtab (ps);
    else if (alt_source_fn != NULL && ps->fullname != NULL &&
             !strcmp (alt_source_fn, ps->filename))
      psymtab_to_symtab (ps);
}

/* Expand all partial symtabs for all source files in an objfile (application,
   library).  In C++, coalesced symbols will end up in an arbitrary symtab,
   so we'll need to expand all of them to find it.  */

static void
expand_all_objfile_psymtabs (struct objfile *obj)
{
  struct partial_symtab *pst = obj->psymtabs;
  ALL_OBJFILE_PSYMTABS_INCL_OBSOLETED (obj, pst)
    psymtab_to_symtab (pst);
}


/* Returns 1 if the file is found.  0 if error or not found.  */

static int
file_exists_p (const char *filename)
{
  struct stat sb;
 
  if (stat (filename, &sb) != 0)
    return 0;

  if ((sb.st_mode & S_IFMT) | S_IFREG ||
      (sb.st_mode & S_IFMT) | S_IFLNK)
    return 1;
  else
    return 0;
}

/* Free all active_func structures in every active_threads structure */

static void
free_active_threads_struct (struct active_threads *head)
{
  struct active_threads *i = head, *j;
  struct active_func *k, *l;

  while (i != NULL)
    {
      k = head->active_func_chain;
      while (k != NULL)
        {
          xfree (k->fi);
          xfree (SYMBOL_NAME (k->sym));
          if (SYMBOL_CPLUS_DEMANGLED_NAME (k->sym))
            xfree (SYMBOL_CPLUS_DEMANGLED_NAME (k->sym));
          xfree (k->sym);
          l = k->next;
          xfree (k);
          k = l;
        }
      j = i->next;
      xfree (i);
      i = j;
    }
}

/* Find the chain of active functions the current thread. */

static struct active_func *
create_current_active_funcs_list (const char *source_filename)
{
  struct active_func * function_chain = NULL;
  struct frame_info *fi;
  struct symbol *sym;
  struct active_func *func;
  struct symtab_and_line sal;
  for (fi = get_current_frame(); fi != 0; fi = get_prev_frame (fi))
    {
      sal = find_pc_line (fi->pc, 0);
      if (sal.symtab == NULL)
        continue;
 
      if ((!strcmp (source_filename, sal.symtab->filename)) ||
          (!strcmp (getbasename (source_filename),
		    getbasename (sal.symtab->filename))))
        {
          sym = find_pc_function (fi->pc);
          if (sym != 0) 
            {
              func = xmalloc (sizeof (struct active_func));
              func->sym = sym;
              func->fi = (struct frame_info *) 
                                     xmalloc (sizeof (struct frame_info));
              memcpy (func->fi, fi, (sizeof (struct frame_info)));

              /* The following copies (and the related free()s in 
                 free_active_threads_struct()) should not be necessary, except
                 that in some circumstances we seem to be accidentally picking
                 up the pre-loaded test objfile, which gets freed shortly
                 hereafter...  */
              func->sym = (struct symbol *)
                                     xmalloc (sizeof (struct symbol));
              memcpy (func->sym, sym, sizeof (struct symbol));
              SYMBOL_NAME (func->sym) = xstrdup (SYMBOL_NAME (sym));
              if (SYMBOL_CPLUS_DEMANGLED_NAME(sym))
                SYMBOL_CPLUS_DEMANGLED_NAME (func->sym) = 
                      xstrdup (SYMBOL_CPLUS_DEMANGLED_NAME (sym));

              if (function_chain == NULL)
		func->next = NULL;
              else
		func->next = function_chain;
              function_chain = func;
            }
        }
    }
  return (function_chain);
}

/* Is a function NAME currently executing? */

static int
in_active_func (const char *name, struct active_threads *threads)
{
  struct active_func *func;
  for (; threads != NULL; threads = threads->next)
    {
      for (func = threads->active_func_chain; func != NULL; func = func->next)
        {
          if (SYMBOL_MATCHES_NAME (func->sym, name))
            return 1;
        }
    }
  return 0;
}

/* Record the value of a memory location, and update it with the new value. */
static void
updatedatum (struct fixinfo *fixinfo, CORE_ADDR addr, char *newval, int size)
{ 
  struct fixeddatum * fixeddatum;
  char buf [8];
  int oldval;
  target_read_memory (addr, buf, size);
  oldval = extract_unsigned_integer (buf, size);
  if (target_write_memory (addr, newval, size))
    error ("Can't redirect function");
  fixeddatum = xmalloc (sizeof (struct fixeddatum));
  fixeddatum->next = NULL;
  fixeddatum->addr = addr;
  fixeddatum->size = size;
  fixeddatum->oldval = oldval;
  fixeddatum->newval = *newval;
  if (fixinfo->most_recent_fix->firstdatum == NULL)
    fixinfo->most_recent_fix->firstdatum = 
                          fixinfo->most_recent_fix->lastdatum = fixeddatum;
  else
    {
       fixinfo->most_recent_fix->lastdatum->next = fixeddatum;
       fixinfo->most_recent_fix->lastdatum = fixeddatum;
    }
}

/* Redirect a function to its new definition, update the gdb
   symbols so the now-obsolete ones are marked as such.  
   This function assumes that checks have already been made to
   assure that the function is large enough to contain the
   trampoline, and that the PC isn't presently in the middle
   of the code we're overwriting.  */

static void
redirect_old_function (struct fixinfo *fixinfo, struct symbol *new_sym, 
                       struct symbol *old_sym, int active)
{
  int inst;
  CORE_ADDR oldfuncstart, oldfuncend, newfuncstart, fixup_addr;
  struct minimal_symbol *msym;
  struct obsoletedsym *obsoletedsym;

  oldfuncstart = BLOCK_START (SYMBOL_BLOCK_VALUE (old_sym));
  oldfuncend = BLOCK_END (SYMBOL_BLOCK_VALUE (old_sym));
  newfuncstart = BLOCK_START (SYMBOL_BLOCK_VALUE (new_sym));

  fixup_addr = oldfuncstart; 

  /* li r12,lo16(newfuncstart) */
  inst = 0x39800000 | encode_lo16 (newfuncstart);
  updatedatum (fixinfo, fixup_addr, (char *)&inst, 4);

  /* addis r12,r12,hi16(newfuncstart) */
  inst = 0x3d8c0000 | encode_hi16 (newfuncstart);
  updatedatum (fixinfo, fixup_addr + 4, (char *)&inst, 4);

  /* mtctr r12  - move contents of r12 (newfuncstart) to count register */
  inst = 0x7d8903a6;
  updatedatum (fixinfo, fixup_addr + 8, (char *)&inst, 4);

  /* bctr - branch unconditionally to count reg, don't update link reg */
  inst = 0x4e800420;
  updatedatum (fixinfo, fixup_addr + 12, (char *)&inst, 4);
  
  /* .long 0 - Illegal instruction for trampoline detection */
  inst = 0x0;
  updatedatum (fixinfo, fixup_addr + 16, (char *)&inst, 4);

  SYMBOL_OBSOLETED (old_sym) = 1;
  msym = lookup_minimal_symbol_by_pc (oldfuncstart);
  if (msym)
    MSYMBOL_OBSOLETED (msym) = 1;
  obsoletedsym = xmalloc (sizeof (struct obsoletedsym)); 
  obsoletedsym->oldsym = old_sym;
  obsoletedsym->newsym = new_sym;
  obsoletedsym->oldmsym = msym;
  msym = lookup_minimal_symbol_by_pc (newfuncstart);
  obsoletedsym->newmsym = msym;
  if (fixinfo->most_recent_fix->obsoletedsym == NULL)
    obsoletedsym->next = NULL; 
  else
    obsoletedsym->next = fixinfo->most_recent_fix->obsoletedsym;
  fixinfo->most_recent_fix->obsoletedsym = obsoletedsym;
}


  /* Detect a Fix and Continue trampoline on PPC systems. 
     Returns 0 if this is not a F&C trampoline; returns the
     destination address if it is.  */

CORE_ADDR
decode_fix_and_continue_trampoline (CORE_ADDR pc)
{
  uint16_t newpc_lo16, newpc_hi16;
  int buf;

  /* li r12,lo16(destination-address) */
  buf = read_memory_unsigned_integer (pc, 4);
  if ((buf & 0x39800000) != 0x39800000)
    return 0;
  newpc_lo16 = buf & 0xffff;

  /* addis r12,r12,hi16(destination-address) */
  buf = read_memory_unsigned_integer (pc + 4, 4);
  if ((buf & 0x3d8c0000) != 0x3d8c0000)
    return 0;
  newpc_hi16 = buf & 0xffff;

  /* mtctr r12 */
  buf = read_memory_unsigned_integer (pc + 8, 4);
  if (buf != 0x7d8903a6)
    return 0;

  /* bctr */
  buf = read_memory_unsigned_integer (pc + 12, 4);
  if (buf != 0x4e800420)
    return 0;

  /* .long 0 */
  buf = read_memory_unsigned_integer (pc + 16, 4);
  if (buf != 0x0)
    return 0;

  return decode_hi16_lo16 (newpc_hi16, newpc_lo16);
}

/* Print all of the functions that are currently on the stack which
   were just replaced, across all threads.  This is only intended for
   MI outputs where the UI can use this list to indicate things to the
   user.  */

static void
print_active_functions (struct fixinfo *cur)
{
  struct active_threads *th;
  struct active_func *fn;
  struct cleanup *uiout_cleanup;

  if (!ui_out_is_mi_like_p (uiout))
    return;

  uiout_cleanup = 
      make_cleanup_ui_out_list_begin_end (uiout, "replaced-functions");
  
  for (th = cur->active_functions; th != NULL; th = th->next)
    {
      struct cleanup *uiout_one_thread_cleanup;
      uiout_one_thread_cleanup = 
           make_cleanup_ui_out_tuple_begin_end (uiout, NULL);
      ui_out_field_int (uiout, "thread-id", th->num);
      make_cleanup_ui_out_list_begin_end (uiout, "replaced");

      for (fn = th->active_func_chain; fn != NULL; fn = fn->next)
          print_frame_info (fn->fi, frame_relative_level (fn->fi), 
                            LOC_AND_ADDRESS, 0);

      do_cleanups (uiout_one_thread_cleanup);
    }

  do_cleanups (uiout_cleanup);
}


/* When doing a fix-and-continue and a replaced function is at frame 0,
   the PC is being moved from an old version of a function to a new version
   via a -thread-set-pc MI command.  Within a function, gcc assumes that the
   PIC base, once set in the prologue, is constant and usable throughout the
   lifetime of the function.  If we change the $pc to point to the new
   version of the function, but do not update the PIC base register, the
   function will soon crash.

   The function that contains the new $pc setting is passed to 
   update_picbase_register as NEW_FUN.
   
   The correct thread should be selected before update_picbase_register is
   called.

   This is entirely MacOS X specific.  */

void
update_picbase_register (struct symbol *new_fun)
{
#if defined (TARGET_POWERPC)
  ppc_function_properties props;
  CORE_ADDR ret;
  int pic_base_reg;
  CORE_ADDR pic_base_value;

  ppc_clear_function_properties (&props);
  ret = ppc_parse_instructions (BLOCK_START (SYMBOL_BLOCK_VALUE (new_fun)),
                                BLOCK_END (SYMBOL_BLOCK_VALUE (new_fun)),
                                &props);

  pic_base_reg = props.pic_base_reg;
  pic_base_value = props.pic_base_address;

  /* FIXME: It is possible to have a function without any PIC base used
     (an empty stub function, like slurry() is in the current fix-small-c
     test case), so for now I'll silently do nothing.  This may not be
     a good choice -- I'm not distinguishing between a function that doesn't
     have a PIC base and a failure to find the PIC base.  */
  if (pic_base_reg == 0 || pic_base_value == INVALID_ADDRESS)
    {
      return;
#if 0
      error ("Unable to find PIC base in new function %s.",
             (new_fun != NULL ? SYMBOL_NAME (new_fun) : "<null>"));
#endif
    }

  write_register (pic_base_reg, pic_base_value);
#endif /* TARGET_POWERPC */
}

static struct objfile *
find_objfile_by_name (const char *name)
{
  struct objfile *obj;

  ALL_OBJFILES (obj)
    if (!strcmp (name, obj->name))
      return obj;

  /* In a cached symfile case, the objfile 'name' member will be the name
     of the cached symfile, not the object file.  The objfile's bfd's filename,
     however, will be the name of the actual object file.  So we'll search
     those as a back-up.  */

  ALL_OBJFILES (obj)
    if (obj->obfd && !strcmp (name, obj->obfd->filename))
      return obj;

  return NULL;
}

/* When we do symbol lookup for a sym in the newly fixed file,
   usually we can find the matching symbol in the symtab (the file)
   that we're replacing/fixing.  With C++, some symbols (inlined
   functions or template functions for example) are emitted in each
   .o file, and the linker coalesces them into one symbol which is
   associated with an arbitrary .o file in the executable.

   So when searching for a symbol with static visibility, if we don't
   find it in the original symtab, it might be one of these coalesced
   symbols and we need to search all the symtabs in the objfile.  

   No special concern is needed for the ZeroLink case -- in that case,
   each source file symtab is its own objfile, and each one will have its
   own copy of all these coalesced items.  

   FIXME: I'm searching the minsyms right now, but it would be more
   reliable to base this off of the partial symtabs.  symtab.c
   doesn't expose either of the two psymtab-searching functions
   globally, though, so for now I'll just use minsyms.
  */

static struct symbol *
search_for_coalesced_symbol (struct objfile *obj, struct symbol *sym)
{
  struct minimal_symbol *minsym;

  minsym = lookup_minimal_symbol (SYMBOL_LINKAGE_NAME (sym), 0, obj);
  if (minsym)
    {
      /* It's in there somewhere... expand symtabs and re-search. */
      expand_all_objfile_psymtabs (obj);

      return (lookup_symbol (SYMBOL_LINKAGE_NAME (sym), NULL,
                              SYMBOL_NAMESPACE (sym), NULL, NULL));
    }

  return (NULL);
}

static void
restore_language (void *arg)
{
  enum language l = (enum language) arg;
  set_language (l);
}

static struct cleanup *
set_current_language (const char *filename)
{
  enum language save_language;
  enum language new_language;

  save_language = current_language->la_language;
  new_language = deduce_language_from_filename (filename);

  if (new_language == save_language)
    return (make_cleanup (null_cleanup, 0));

  set_language (new_language);

  return (make_cleanup (restore_language, (void *) save_language));
}

/* Given the SRC_FILENAME and SRC_BASENAME fields of the CUR structure
   being set already, scan through all known source files to determine
   which object file contains the *original* version of this source file.
   Updates the CUR structure with the correct filename.

   The ORIGINAL_OBJFILE_FILENAME is malloc()'ed here, so it should be
   freed if the structure is ever freed.  CANONICAL_SOURCE_FILENAME is
   just a pointer to one of the other filenames so it should not be freed.  */

static void
find_original_object_file_name (struct fixinfo *cur)
{
  struct objfile *obj;
  struct partial_symtab *ps;

  if (cur->original_objfile_filename != NULL &&
      cur->canonical_source_filename != NULL)
    return;

  ALL_PSYMTABS (obj, ps)
    if (!strcmp (ps->filename, cur->src_filename) ||
        (ps->fullname != NULL && !strcmp (ps->fullname, cur->src_filename)))
      {
        /* FIXME: The cond is to guard against a probably bug in the Apple
           gdb sources where we have two psymtabs, 2003-03-17/jmolenda */
        if (ps->texthigh != 0 &&
            (strcmp (ps->objfile->name, cur->bundle_filename) != 0))
          {
            psymtab_to_symtab (ps);
            cur->original_objfile_filename = xstrdup (obj->name);
            cur->canonical_source_filename = cur->src_filename;
            return;
          }
      }

  /* Now try searching for just the filename without the path.  There is
     a good chance that this could match the wrong file, but we'll use it
     as a backup scheme. */

  ALL_PSYMTABS (obj, ps)
    if (!strcmp (ps->filename, cur->src_basename) ||
        (ps->fullname != NULL && !strcmp (ps->fullname, cur->src_basename)))
      {
        /* FIXME: The cond is to guard against a probably bug in the Apple
           gdb sources where we have two psymtabs, 2003-03-17/jmolenda */
        if (ps->texthigh != 0 && 
            (strcmp (ps->objfile->name, cur->bundle_filename) != 0))
          {
            psymtab_to_symtab (ps);
            cur->original_objfile_filename = xstrdup (obj->name);
            cur->canonical_source_filename = cur->src_basename;
            return;
          }
      }

  cur->original_objfile_filename = NULL;
  cur->canonical_source_filename = NULL;
  error ("Unable to find original source file %s.  "
         "Target built without debugging symbols?", cur->src_basename);
}
  
static struct objfile *
find_original_object_file (struct fixinfo *cur)
{
  if (cur->original_objfile_filename == NULL)
    error ("find_original_object_file() called with an empty filename!");

  return find_objfile_by_name (cur->original_objfile_filename);
}

static struct symtab *
find_original_symtab (struct fixinfo *cur)
{
  return PSYMTAB_TO_SYMTAB (find_original_psymtab (cur));
}

static struct partial_symtab *
find_original_psymtab (struct fixinfo *cur)
{
  struct objfile *original_objfile;
  struct partial_symtab *pst;

  original_objfile = find_original_object_file (cur);

  if (original_objfile == NULL)
    error ("Unable to find original object file!");

  ALL_OBJFILE_PSYMTABS_INCL_OBSOLETED (original_objfile, pst)
    if (!strcmp (pst->filename, cur->canonical_source_filename))
      return pst;

  error ("Unable to find original source file '%s'!  "
         "Target compiled without debug information?", 
         cur->canonical_source_filename);
}

void
_initialize_fix (void)
{
  struct cmd_list_element *c;

  c = add_com ("fix", class_files, fix_command, "Bring in a fixed objfile.");
  set_cmd_completer (c, filename_completer);

  c = add_set_cmd ("fix-and-continue", class_obscure, var_boolean,
      (char *) &fix_and_continue_debug_flag,
      "Set if GDB prints debug information while Fix and Continuing.",
      &setdebuglist);
  add_show_from_set (c, &showdebuglist);
}