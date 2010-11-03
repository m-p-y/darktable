/*
		This file is part of darktable,
		copyright (c) 2009--2010 johannes hanika.

		darktable is free software: you can redistribute it and/or modify
		it under the terms of the GNU General Public License as published by
		the Free Software Foundation, either version 3 of the License, or
		(at your option) any later version.

		darktable is distributed in the hope that it will be useful,
		but WITHOUT ANY WARRANTY; without even the implied warranty of
		MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
		GNU General Public License for more details.

		You should have received a copy of the GNU General Public License
		along with darktable.  If not, see <http://www.gnu.org/licenses/>.
*/
#include "control/control.h"
#include "control/conf.h"
#include "control/jobs.h"
#include "common/film.h"
#include "common/collection.h"
#include "views/view.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <assert.h>

void dt_film_init(dt_film_t *film)
{
	pthread_mutex_init(&film->images_mutex, NULL);
	film->last_loaded = film->num_images = 0;
	film->dirname[0] = '\0';
	film->dir = NULL;
	film->id = -1;
	film->ref = 0;
}

void dt_film_cleanup(dt_film_t *film)
{
	pthread_mutex_destroy(&film->images_mutex);
	if(film->dir)
	{
		g_dir_close(film->dir);
		film->dir = NULL;
	}
	// if the film is empty => remove it again.
	if(dt_film_is_empty(film->id))
  {
		dt_film_remove(film->id);
	}
	else
	{
		dt_control_update_recent_films();
	}
}

void dt_film_set_query(const int32_t id)
{
	/* enable film id filter and set film id */
	dt_collection_set_query_flags (darktable.collection, COLLECTION_QUERY_FULL);
	dt_collection_set_filter_flags (darktable.collection, (dt_collection_get_filter_flags (darktable.collection) | COLLECTION_FILTER_FILM_ID) );
	dt_collection_set_film_id (darktable.collection, id);
	dt_collection_update (darktable.collection);
	dt_conf_set_int ("ui_last/film_roll", id);
}

/** open film with given id. */
int 
dt_film_open2 (dt_film_t *film)
{
	/* check if we got a decent film id */
	if(film->id<0) return 1;
	
	/* query database for id and folder */
	int rc;
	sqlite3_stmt *stmt;
	rc = sqlite3_prepare_v2(darktable.db, "select id, folder from film_rolls where id = ?1", -1, &stmt, NULL);
	HANDLE_SQLITE_ERR(rc);
	rc = sqlite3_bind_int(stmt, 1, film->id);
	HANDLE_SQLITE_ERR(rc);
	if(sqlite3_step(stmt) == SQLITE_ROW)
	{
		/* fill out the film dirname */
		sprintf (film->dirname,"%s",(gchar *)sqlite3_column_text (stmt, 1));
		rc = sqlite3_finalize (stmt);
		char datetime[20];
		dt_gettime (datetime);

		rc = sqlite3_prepare_v2 (darktable.db, "update film_rolls set datetime_accessed = ?1 where id = ?2", -1, &stmt, NULL);
		HANDLE_SQLITE_ERR (rc);
		rc = sqlite3_bind_text (stmt, 1, datetime, strlen(datetime), SQLITE_STATIC);
		rc = sqlite3_bind_int (stmt, 2, film->id);
		HANDLE_SQLITE_ERR (rc);
		sqlite3_step (stmt);
		
		rc = sqlite3_finalize (stmt);
		dt_control_update_recent_films ();
		dt_film_set_query (film->id);
		dt_control_queue_draw_all ();
		dt_view_manager_reset (darktable.view_manager);
		return 0;
	}
	
	/* failure */
	return 1;
}

int dt_film_open(const int32_t id)
{
	int rc;
	sqlite3_stmt *stmt;
	rc = sqlite3_prepare_v2(darktable.db, "select id, folder from film_rolls where id = ?1", -1, &stmt, NULL);
	HANDLE_SQLITE_ERR(rc);
	rc = sqlite3_bind_int(stmt, 1, id);
	HANDLE_SQLITE_ERR(rc);
	if(sqlite3_step(stmt) == SQLITE_ROW)
	{
		// FIXME: this is a hack to synch the duplicate gui elements all film rolls/collect by film roll:
		dt_conf_set_string("plugins/lighttable/collect/string", (gchar *)sqlite3_column_text(stmt, 1));
		dt_conf_set_int ("plugins/lighttable/collect/item", 0);
		rc = sqlite3_finalize(stmt);
		char datetime[20];
		dt_gettime(datetime);

		rc = sqlite3_prepare_v2(darktable.db, "update film_rolls set datetime_accessed = ?1 where id = ?2", -1, &stmt, NULL);
		HANDLE_SQLITE_ERR(rc);
		rc = sqlite3_bind_text(stmt, 1, datetime, strlen(datetime), SQLITE_STATIC);
		rc = sqlite3_bind_int (stmt, 2, id);
		HANDLE_SQLITE_ERR(rc);
		sqlite3_step(stmt);
	}
	rc = sqlite3_finalize(stmt);
	// TODO: prefetch to cache using image_open
	dt_control_update_recent_films();
	dt_film_set_query(id);
	dt_control_queue_draw_all();
	dt_view_manager_reset(darktable.view_manager);
	return 0;
}

int dt_film_open_recent(const int num)
{
	int32_t rc;
	sqlite3_stmt *stmt;
	rc = sqlite3_prepare_v2(darktable.db, "select id from film_rolls order by datetime_accessed desc limit ?1,1", -1, &stmt, NULL);
	HANDLE_SQLITE_ERR(rc);
	rc = sqlite3_bind_int (stmt, 1, num);
	HANDLE_SQLITE_ERR(rc);
	if(sqlite3_step(stmt) == SQLITE_ROW)
	{
		int id = sqlite3_column_int(stmt, 0);
		sqlite3_finalize(stmt);
		if(dt_film_open(id)) return 1;
		char datetime[20];
		dt_gettime(datetime);
		rc = sqlite3_prepare_v2(darktable.db, "update film_rolls set datetime_accessed = ?1 where id = ?2", -1, &stmt, NULL);
		HANDLE_SQLITE_ERR(rc);
		rc = sqlite3_bind_text(stmt, 1, datetime, strlen(datetime), SQLITE_STATIC);
		rc = sqlite3_bind_int (stmt, 2, id);
		HANDLE_SQLITE_ERR(rc);
		sqlite3_step(stmt);
	}
	rc = sqlite3_finalize(stmt);
	dt_control_update_recent_films();
	return 0;
}

int dt_film_new(dt_film_t *film,const char *directory)
{
	// Try open filmroll for folder if exists
	film->id = -1;
	int rc;
	sqlite3_stmt *stmt;
	rc = sqlite3_prepare_v2(darktable.db, "select id from film_rolls where folder = ?1", -1, &stmt, NULL);
	HANDLE_SQLITE_ERR(rc);
	rc = sqlite3_bind_text(stmt, 1, directory, strlen(directory), SQLITE_STATIC);
	HANDLE_SQLITE_ERR(rc);
	if(sqlite3_step(stmt) == SQLITE_ROW) film->id = sqlite3_column_int(stmt, 0);
	rc = sqlite3_finalize(stmt);
	
	if(film->id <= 0)
	{ // create a new filmroll
		int rc;
		sqlite3_stmt *stmt;
		char datetime[20];
		dt_gettime(datetime);
		rc = sqlite3_prepare_v2(darktable.db, "insert into film_rolls (id, datetime_accessed, folder) values (null, ?1, ?2)", -1, &stmt, NULL);
		HANDLE_SQLITE_ERR(rc);
		rc = sqlite3_bind_text(stmt, 1, datetime, strlen(datetime), SQLITE_STATIC);
		rc = sqlite3_bind_text(stmt, 2, directory, strlen(directory), SQLITE_STATIC);
		HANDLE_SQLITE_ERR(rc);
		pthread_mutex_lock(&(darktable.db_insert));
		rc = sqlite3_step(stmt);
		if(rc != SQLITE_DONE) fprintf(stderr, "[film_import] failed to insert film roll! %s\n", sqlite3_errmsg(darktable.db));
		rc = sqlite3_finalize(stmt);
		sqlite3_prepare_v2(darktable.db, "select id from film_rolls where folder=?1", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, directory, strlen(directory), SQLITE_STATIC);
    if(sqlite3_step(stmt) == SQLITE_ROW) film->id = sqlite3_column_int(stmt, 0);
	  sqlite3_finalize(stmt);
		pthread_mutex_unlock(&(darktable.db_insert));
	}
	
	if(film->id<=0)
			return 0;
	strcpy(film->dirname,directory);
	film->last_loaded = 0;
	return film->id;
}

void dt_film_image_import(dt_film_t *film,const char *filename)
{ // import an image into filmroll
	 if(dt_image_import(film->id, filename)) 
		 dt_control_queue_draw_all();
}

static int
dt_film_import_blocking(const char *dirname, const int blocking)
{
	// init film and give each thread a pointer, last one cleans up.
	dt_film_t *film = (dt_film_t *)malloc(sizeof(dt_film_t));
	dt_film_init(film);
	film->id = -1;
	int rc;
	sqlite3_stmt *stmt;
	rc = sqlite3_prepare_v2(darktable.db, "select id from film_rolls where folder = ?1", -1, &stmt, NULL);
	HANDLE_SQLITE_ERR(rc);
	rc = sqlite3_bind_text(stmt, 1, dirname, strlen(dirname), SQLITE_STATIC);
	HANDLE_SQLITE_ERR(rc);
	if(sqlite3_step(stmt) == SQLITE_ROW) film->id = sqlite3_column_int(stmt, 0);
	rc = sqlite3_finalize(stmt);
	if(film->id <= 0)
	{
		// insert timestamp
		char datetime[20];
		dt_gettime(datetime);
		rc = sqlite3_prepare_v2(darktable.db, "insert into film_rolls (id, datetime_accessed, folder) values (null, ?1, ?2)", -1, &stmt, NULL);
		HANDLE_SQLITE_ERR(rc);
		rc = sqlite3_bind_text(stmt, 1, datetime, strlen(datetime), SQLITE_STATIC);
		rc = sqlite3_bind_text(stmt, 2, dirname, strlen(dirname), SQLITE_STATIC);
		HANDLE_SQLITE_ERR(rc);
		rc = sqlite3_step(stmt);
		if(rc != SQLITE_DONE) fprintf(stderr, "[film_import] failed to insert film roll! %s\n", sqlite3_errmsg(darktable.db));
		rc = sqlite3_finalize(stmt);
		sqlite3_prepare_v2(darktable.db, "select id from film_rolls where folder=?1", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, dirname, strlen(dirname), SQLITE_STATIC);
    if(sqlite3_step(stmt) == SQLITE_ROW) film->id = sqlite3_column_int(stmt, 0);
	  sqlite3_finalize(stmt);
	}
	if(film->id <= 0)
	{
		dt_film_cleanup(film);
		free(film);
		return 0;
	}

	film->last_loaded = 0;
	strncpy(film->dirname, dirname, 512);
	film->dir = g_dir_open(film->dirname, 0, NULL);

	// TODO: set film->num_images for progress bar!

	const int ret = film->id;
  if(blocking)
  {
    dt_film_import1(film);
    dt_film_cleanup(film);
    free(film);
  }
  else
  {
    // darktable.control->progress = .001f;
    // not more than one job: recursive import is not thread-safe, and multiple
    // threads accessing the harddisk at once is not a good idea performance wise.
    // for(int k=0;k<MAX(1,dt_ctl_get_num_procs());k++)
    {
      // last job will destroy film struct.
      dt_job_t j;
      dt_film_import1_init(&j, film);
      dt_control_add_job(darktable.control, &j);
    }
  }
	return ret;
}

//FIXME: recursion messes up the progress counter.
void dt_film_import1(dt_film_t *film)
{
	const gchar *d_name;
	char filename[1024];
	dt_image_t image;

	gboolean recursive = dt_conf_get_bool("ui_last/import_recursive");

	while(1)
	{
		pthread_mutex_lock(&film->images_mutex);
		if (film->dir && (d_name = g_dir_read_name(film->dir)) && dt_control_running())
		{
			snprintf(filename, 1024, "%s/%s", film->dirname, d_name);
			image.film_id = film->id;
			film->last_loaded++;
		}
		else
		{
			if(film->dir)
			{
				g_dir_close(film->dir);
				film->dir = NULL;
			}
			darktable.control->progress = 200.0f;
			pthread_mutex_unlock(&film->images_mutex);
			return;
		}
		pthread_mutex_unlock(&film->images_mutex);

		if(recursive && g_file_test(filename, G_FILE_TEST_IS_DIR))
		{ // import in this thread (recursive import is not thread-safe):
			dt_film_import_blocking(filename, 1);
		}
		else if(dt_image_import(film->id, filename))
		{
			pthread_mutex_lock(&film->images_mutex);
			darktable.control->progress = 100.0f*film->last_loaded/(float)film->num_images;
			pthread_mutex_unlock(&film->images_mutex);
			dt_control_queue_draw_all();
		} // else not an image.
	}
}

int dt_film_import(const char *dirname)
{
  return dt_film_import_blocking(dirname, 0);
}

int dt_film_is_empty(const int id)
{
	int rc, empty=0;
	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(darktable.db, "select id from images where film_id = ?1", -1, &stmt, NULL);
	sqlite3_bind_int(stmt, 1, id);
	if( sqlite3_step(stmt) != SQLITE_ROW) empty=1;
	rc = sqlite3_finalize(stmt);
	return empty;
}

void dt_film_remove(const int id)
{
	int rc;
	sqlite3_stmt *stmt;
	sqlite3_prepare_v2(darktable.db, "select id from images where film_id = ?1", -1, &stmt, NULL);
	sqlite3_bind_int(stmt, 1, id);
	while(sqlite3_step(stmt) == SQLITE_ROW)
		dt_image_remove(sqlite3_column_int(stmt, 0));
	rc = sqlite3_finalize(stmt);

	rc = sqlite3_prepare_v2(darktable.db, "delete from film_rolls where id = ?1", -1, &stmt, NULL);
	rc = sqlite3_bind_int(stmt, 1, id);
	rc = sqlite3_step(stmt);
	rc = sqlite3_finalize(stmt);
	dt_control_update_recent_films();
}

