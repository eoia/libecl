/*
   Copyright (C) 2011  Statoil ASA, Norway. 
    
   The file 'well_state.c' is part of ERT - Ensemble based Reservoir Tool. 
    
   ERT is free software: you can redistribute it and/or modify 
   it under the terms of the GNU General Public License as published by 
   the Free Software Foundation, either version 3 of the License, or 
   (at your option) any later version. 
    
   ERT is distributed in the hope that it will be useful, but WITHOUT ANY 
   WARRANTY; without even the implied warranty of MERCHANTABILITY or 
   FITNESS FOR A PARTICULAR PURPOSE.   
    
   See the GNU General Public License at <http://www.gnu.org/licenses/gpl.html> 
   for more details. 
*/

/**
   The well_state_type structure contains state information about one
   well for one particular point in time.
*/


#include <time.h>
#include <stdbool.h>

#include <util.h>
#include <vector.h>
#include <hash.h>
#include <int_vector.h>
#include <ecl_intehead.h>
#include <ecl_file.h>
#include <ecl_kw.h>
#include <ecl_kw_magic.h>
#include <ecl_util.h>

#include <well_const.h>
#include <well_conn.h>
#include <well_state.h>
#include <well_path.h>

#define GLOBAL_GRID_NAME   "GLOBAL" 
#define WELL_STATE_TYPE_ID 613307832

struct well_state_struct {
  UTIL_TYPE_ID_DECLARATION;
  char           * name;
  time_t           valid_from_time;
  int              valid_from_report;
  bool             open;
  well_conn_type * wellhead;
  well_type_enum   type;

  vector_type    * index_lgr_path;   // Contains the various well_path instances indexed by grid_nr - global has grid_nr == 0.
  hash_type      * name_lgr_path;    // Contains the different well_path instances indexed by lgr_name  
};



UTIL_IS_INSTANCE_FUNCTION( well_state , WELL_STATE_TYPE_ID)


static well_state_type * well_state_alloc_empty() {
  well_state_type * well_state = util_malloc( sizeof * well_state , __func__ );
  UTIL_TYPE_ID_INIT( well_state , WELL_STATE_TYPE_ID );
  well_state->index_lgr_path = vector_alloc_new();
  well_state->name_lgr_path  = hash_alloc();
  return well_state;
}

well_path_type * well_state_add_path( well_state_type * well_state , const ecl_file_type * ecl_file , int grid_nr) {
  well_path_type * well_path;
  char * grid_name = util_alloc_string_copy( GLOBAL_GRID_NAME );
  if (grid_nr > 0) {
    const ecl_kw_type * lgr_kw = ecl_file_iget_named_kw( ecl_file , LGR_KW , 0 );
    grid_name = util_alloc_strip_copy(ecl_kw_iget_ptr( lgr_kw , grid_nr -1 ));  
  }
  well_path = well_path_alloc( grid_name , (grid_nr == 0) ? true : false);
    
  vector_safe_iset_owned_ref( well_state->index_lgr_path , grid_nr , well_path , well_path_free__);
  hash_insert_ref( well_state->name_lgr_path , grid_name , well_path );
  free( grid_name );

  return well_path;
}




static void well_state_add_connections( well_state_type * well_state ,  const ecl_file_type * ecl_file , int grid_nr, int well_nr ) {
  ecl_intehead_type * header   = ecl_intehead_alloc( ecl_file_iget_named_kw( ecl_file , INTEHEAD_KW , grid_nr ));
  const ecl_kw_type * icon_kw  = ecl_file_iget_named_kw( ecl_file , ICON_KW   , grid_nr);
  const ecl_kw_type * iwel_kw  = ecl_file_iget_named_kw( ecl_file , IWEL_KW   , grid_nr);
  const int iwel_offset        = header->niwelz * well_nr;
  int num_connections          = ecl_kw_iget_int( iwel_kw , iwel_offset + IWEL_CONNECTIONS_ITEM );
  ecl_kw_type * iseg_kw        = NULL;
  bool MSW                     = false;   // MultiSegmentWell
  well_path_type * path        = well_state_add_path(well_state , ecl_file , grid_nr);
  int seg_well_nr              = ecl_kw_iget_int( iwel_kw , iwel_offset + IWEL_SEGMENTED_WELL_NR_ITEM) - 1; // -1: Ordinary well.
  
  if (seg_well_nr >= 0)
    MSW = true;
  
  if (MSW)
    iseg_kw = ecl_file_iget_named_kw( ecl_file , ISEG_KW , grid_nr );
  
  for (int conn_nr = 0; conn_nr < num_connections; conn_nr++) {
    well_conn_type * conn =  well_conn_alloc( icon_kw , iseg_kw , header , well_nr , seg_well_nr , conn_nr );
    well_path_add_conn( path , conn );
  }
  ecl_intehead_free( header );
}


static int well_state_get_lgr_well_nr( const well_state_type * well_state , const ecl_file_type * ecl_file , int grid_nr ) {
  printf("Looking for well_nr  well:%s  grid:%d \n",well_state->name , grid_nr);
  const ecl_kw_type * zwel_kw = ecl_file_iget_named_kw( ecl_file , ZWEL_KW   , grid_nr);
  int num_lgr_wells = ecl_kw_get_size( zwel_kw );
  int well_nr = 0;
  while (true) {
    bool found = false;

    {
      char * lgr_well_name = util_alloc_strip_copy( ecl_kw_iget_ptr( zwel_kw , well_nr) );
      if ( strcmp( well_state->name , lgr_well_name) == 0)
        found = true;
      else
        well_nr++;
      
      free( lgr_well_name );
    }
    if (found)
      break;
    else if (well_nr == num_lgr_wells) {
      // The well is not in this LGR at all.
      well_nr = -1;
      break;
    }
  }
  return well_nr;
}

// This is misleading because a grid can only be completed in one grid, i.e.
// either the global grid or one of the LGRs. The only exception to this is
// when several LGRs are amalgameted to one 'super' lgr.

well_state_type * well_state_alloc( const ecl_file_type * ecl_file , int report_nr ,  int global_well_nr) {
  well_state_type * well_state = NULL;
  ecl_intehead_type * global_header  = ecl_intehead_alloc( ecl_file_iget_named_kw( ecl_file , INTEHEAD_KW , 0 ));
  const ecl_kw_type * global_iwel_kw = ecl_file_iget_named_kw( ecl_file , IWEL_KW   , 0);
  const ecl_kw_type * global_zwel_kw = ecl_file_iget_named_kw( ecl_file , ZWEL_KW   , 0);

  const int iwel_offset = global_header->niwelz * global_well_nr;
  {
    well_state = well_state_alloc_empty();
    
    const int zwel_offset         = global_header->nzwelz * global_well_nr;
    
    well_state->valid_from_time   = global_header->sim_time;
    well_state->valid_from_report = report_nr;
    well_state->wellhead          = well_conn_alloc_wellhead( global_iwel_kw , global_header , global_well_nr );
    well_state->name              = util_alloc_strip_copy(ecl_kw_iget_ptr( global_zwel_kw , zwel_offset ));  // Hardwired max 8 characters in Well Name
    
    {
      int int_state = ecl_kw_iget_int( global_iwel_kw , iwel_offset + IWEL_STATUS_ITEM );
      if (int_state > 0)
        well_state->open = true;
      else
        well_state->open = false;
    }

    {
      int int_type = ecl_kw_iget_int( global_iwel_kw , iwel_offset + IWEL_TYPE_ITEM);
      switch (int_type) {
      /* See documentation of the 'IWEL_UNDOCUMENTED_ZERO' in well_const.h */
      case(IWEL_UNDOCUMENTED_ZERO):
        well_state->type = UNDOCUMENTED_ZERO;
        if (well_state->open)
          util_abort("%s: Invalid type value %d\n",__func__ , int_type);
        break;
      case(IWEL_PRODUCER):
        well_state->type = PRODUCER;
        break;
      case(IWEL_OIL_INJECTOR):
        well_state->type = OIL_INJECTOR;
        break;
      case(IWEL_GAS_INJECTOR):
        well_state->type = GAS_INJECTOR;
        break;
      case(IWEL_WATER_INJECTOR):
        well_state->type = WATER_INJECTOR;
        break;
      default:
        util_abort("%s: Invalid type value %d\n",__func__ , int_type);
      }
    }
    

    
    // Go through all the LGRs and add connections.
    
    /*
      This code block is unfortunately not complete/correct; it turns
      out that in the case of nested LGRs the LGR based IWEL
      information for all the amalgamated LGRS is assembled in one
      IWEL keyword. The current code assumes a strict one IWEL pr LGR
      structure.
    */
    {
      int num_grid = ecl_file_get_num_named_kw( ecl_file , INTEHEAD_KW );
      for (int grid_nr = 0; grid_nr < num_grid; grid_nr++) {
        int well_nr;
        printf("grid:%d \n",grid_nr);
        if (grid_nr == 0)
          well_nr = global_well_nr;
        else
          well_nr = well_state_get_lgr_well_nr( well_state , ecl_file , grid_nr );
        
        if (well_nr >= 0) 
          well_state_add_connections( well_state , ecl_file , grid_nr , well_nr );
      }
    }
    
    
  } 
  ecl_intehead_free( global_header );
  return well_state;
}


void well_state_free( well_state_type * well ) {
  well_conn_free( well->wellhead );
  hash_free( well->name_lgr_path );
  vector_free( well->index_lgr_path );
  free( well->name );
  free( well );
}

/*****************************************************************/

int well_state_get_report_nr( const well_state_type * well_state ) {
  return well_state->valid_from_report;
}

time_t well_state_get_sim_time( const well_state_type * well_state ) {
  return well_state->valid_from_time;
}


well_conn_type * well_get_wellhead( const well_state_type * well_state ) {
  return well_state->wellhead;
}


well_type_enum well_state_get_type( const well_state_type * well_state){ 
  return well_state->type;
}

bool well_state_is_open( const well_state_type * well_state ) {
  return well_state->open;
}

const char * well_state_get_name( const well_state_type * well_state ) {
  return well_state->name;
}

/*****************************************************************/

well_path_type * well_state_get_path( const well_state_type * well_state , const char * lgr_name) {
  return hash_get( well_state->name_lgr_path , lgr_name );
}

well_path_type * well_state_iget_path( const well_state_type * well_state , int grid_nr) {
  return vector_safe_iget( well_state->index_lgr_path , grid_nr );
}

const well_conn_type ** well_state_iget_lgr_connections(const well_state_type * well_state , int grid_nr , int branch_nr ) {
  well_path_type * well_path = well_state_iget_path( well_state , grid_nr );
  well_branch_type * branch = well_path_iget_branch( well_path , branch_nr );
  if (branch != NULL)
    return well_branch_get_connections( branch );
  else
    return NULL; // Branch does not exist - or has 0 connections.
}

const well_conn_type ** well_state_get_lgr_connections(const well_state_type * well_state , const char * lgr_name , int branch_nr) {
  well_path_type * well_path = well_state_get_path( well_state , lgr_name );
  well_branch_type * branch = well_path_iget_branch( well_path , branch_nr );
  if (branch != NULL)
    return well_branch_get_connections( branch );
  else
    return NULL; // Branch does not exist - or has 0 connections.
}


const well_conn_type ** well_state_get_connections(const well_state_type * well_state , int branch_nr ) {
  return well_state_iget_lgr_connections(well_state , 0 , branch_nr );
} 

/*****************************************************************/

int well_state_iget_num_lgr_connections(const well_state_type * well_state , int grid_nr , int branch_nr ) {
  well_path_type * well_path = well_state_iget_path( well_state , grid_nr );
  well_branch_type * branch = well_path_iget_branch( well_path , branch_nr );
  if (branch != NULL)
    return well_branch_get_length( branch );
  else
    return 0;
}

int well_state_get_num_lgr_connections(const well_state_type * well_state , const char * lgr_name , int branch_nr) {
  well_path_type * well_path = well_state_get_path( well_state , lgr_name );
  well_branch_type * branch = well_path_iget_branch( well_path , branch_nr );
  if (branch != NULL)
    return well_branch_get_length( branch );
  else
    return 0;
}


int well_state_get_num_connections(const well_state_type * well_state , int branch_nr ) {
  return well_state_iget_num_lgr_connections(well_state , 0 , branch_nr );
}

/*****************************************************************/

int well_state_iget_lgr_num_branches( const well_state_type * well_state , int grid_nr) {
  well_path_type * well_path = well_state_iget_path( well_state , grid_nr );
  return well_path_get_max_branches( well_path );
}

int well_state_get_lgr_num_branches( const well_state_type * well_state , const char * lgr_name) {
  well_path_type * well_path = well_state_get_path( well_state , lgr_name );
  return well_path_get_max_branches( well_path );
}

int well_state_get_num_branches(const well_state_type * well_state ) {
  return well_state_iget_lgr_num_branches( well_state , 0 );
}

/*****************************************************************/

int well_state_get_num_paths( const well_state_type * well_state ) {
  return vector_get_size( well_state->index_lgr_path );
}

/*****************************************************************/

void well_state_summarize( const well_state_type * well_state , FILE * stream ) {
  fprintf(stream , "Well: %s \n" , well_state->name );
  {
    for (int grid_nr=0; grid_nr < well_state_get_num_paths( well_state ); grid_nr++) {
      well_path_type * well_path = well_state_iget_path(well_state , grid_nr );
      if (well_path != NULL) {
        fprintf(stream , "   Grid: %-8s\n",well_path_get_grid_name( well_path ));
        {
          int num_branches = well_path_get_max_branches(well_path);
          for (int branch_nr = 0; branch_nr < num_branches; branch_nr++) {
            well_branch_type * branch = well_path_iget_branch( well_path , branch_nr );
            if (branch != NULL) {
              const well_conn_type ** connections = well_branch_get_connections( branch );
              int num_connections = well_branch_get_length( branch );
              fprintf(stream , "      Branch %2d: [" , branch_nr );
              for (int iconn=0; iconn < num_connections; iconn++) {
                const well_conn_type * conn = connections[ iconn ];
                fprintf(stream, "(%2d,%2d,%2d)",conn->i,conn->j,conn->k);
                if (iconn == (num_connections - 1))
                  fprintf(stream , "]\n");
                else {
                  fprintf(stream , ", ");
                  if (iconn % 10 == 0)
                    fprintf(stream , "\n                  ");
                }
              }
            }
          }
        }
      }
    }
  }
}
