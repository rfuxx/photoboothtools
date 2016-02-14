/* 
 * Originally based on sample-tether.c from libgphoto2
 * This program does the equivalent of:
 * gphoto2 --wait-event-and-download
 * plus the following extra features:
 * - queues downloads so that jpg files are downloaded first
 *   (in case you shoot raw+jpg this will give you the viewable files faster,
 *   and the raw file is reordered for later download)
 * - renames downloads according to exif date
 * - rotates jpegs automatically (and handles exif similar as with exiftran)
 *
 */

#include <unistd.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <pthread.h>
#include <sys/queue.h>
#include <jpeglib.h>
#include <gphoto2/gphoto2.h>
#include <libexif/exif-data.h>
#include <libraw/libraw.h>
#include <libraw/libraw_version.h>
#include "transupp/transupp.h"

char *receivedir;
libraw_data_t *libraw;

struct jpeg_info {
	char *camerafilename;
	int fd_from_gphoto;
	FILE *src;
	FILE *dest;
	int transform;
};

static void errordumper(GPLogLevel level, const char *domain, const char *str, void *data) {
	fprintf(stderr, "ERROR: %s\n", str);
}

static char *unique_filename(char *filename) {
	struct stat st;
	char *result, *basename, *ext;
	int num;

	result = malloc(strlen(filename) + 10);
	strcpy(result, filename);
	basename = malloc(strlen(filename) +1);
	strcpy(basename, filename);
	ext = "";
	for(num=strlen(basename)-1; num >= 0; num--) {
		if(basename[num] == '.') {
			ext = &basename[num+1];
			basename[num] = '\0';
			break;
		}
	}
	num = 0;
	while(stat(result, &st) == 0) {
		num++;
		sprintf(result, "%s-%d.%s", basename, num, ext);
	}
	free(basename);
	return result;
}

static void set_exif_int(ExifData *ed, ExifEntry *ee, long value) {
	ExifByteOrder o = exif_data_get_byte_order(ed);

	switch (ee->format) {
		case EXIF_FORMAT_SHORT:
			exif_set_short(ee->data, o, value);
			break;
		case EXIF_FORMAT_LONG:
			exif_set_long(ee->data, o, value);
			break;
		case EXIF_FORMAT_SLONG:
			exif_set_slong(ee->data, o, value);
			break;
		default:
			fprintf(stderr,"ExifByteOrder unknown/unhandled format %d for int\n", ee->format);
	}
}

static void update_exif_dimension(ExifData *ed, int transform, int src_x, int src_y) {
	static struct {
		int idf;
		int tag;
		int x;
	} fields[] = {
		{
			.idf = EXIF_IFD_EXIF,
			.tag = EXIF_TAG_PIXEL_X_DIMENSION,
			.x   = 1,
		},{
			.idf = EXIF_IFD_EXIF,
			.tag = EXIF_TAG_PIXEL_Y_DIMENSION,
			.x   = 0,
		},{
			.idf = EXIF_IFD_INTEROPERABILITY,
			.tag = EXIF_TAG_RELATED_IMAGE_WIDTH,
			.x   = 1,
		},{
			.idf = EXIF_IFD_INTEROPERABILITY,
			.tag = EXIF_TAG_RELATED_IMAGE_LENGTH,
			.x   = 0,
		}
	};
	ExifEntry *ee;
	int i;

	for (i = 0; i < sizeof(fields)/sizeof(fields[0]); i++) {
		ee = exif_content_get_entry(ed->ifd[fields[i].idf], fields[i].tag);
		if (ee != NULL) {
			switch (transform) {
				case JXFORM_ROT_90:
				case JXFORM_ROT_270:
				case JXFORM_TRANSPOSE:
				case JXFORM_TRANSVERSE:
					/* swap width/height */
					set_exif_int(ed, ee, fields[i].x ? src_y : src_x);
					break;
				default:
					/* original dimensions */
					set_exif_int(ed, ee, fields[i].x ? src_x : src_y);
					break;
			}
		}
	}
}

static void jpegtran_do_transform(struct jpeg_decompress_struct *src, struct jpeg_compress_struct *dst, int transform);

static void update_exif_thumbnail(ExifData *ed, int transform) {
	struct jpeg_decompress_struct src;
	struct jpeg_compress_struct dst;
	struct jpeg_error_mgr jsrcerr, jdsterr;
	unsigned char *newthumb;
	unsigned long newthumbsize;
        newthumb = NULL;
        newthumbsize = 0;

	src.err = jpeg_std_error(&jsrcerr);
	jpeg_create_decompress(&src);
	jpeg_mem_src(&src, ed->data, ed->size);

	dst.err = jpeg_std_error(&jdsterr);
	jpeg_create_compress(&dst);
	jpeg_mem_dest(&dst, &newthumb, &newthumbsize);

	jpegtran_do_transform(&src, &dst, transform);

	free(ed->data);
	ed->data = newthumb;
	ed->size = newthumbsize;
}

static void transform_exif(struct jpeg_decompress_struct *srcinfo, int transform) {
	jpeg_saved_marker_ptr mark;
	ExifData *ed = NULL;
	ExifEntry *ee;
	unsigned char *data;
	unsigned int size;

	for(mark = srcinfo->marker_list; NULL != mark; mark = mark->next) {
		if (mark->marker == JPEG_APP0 +1) {
			ed = exif_data_new_from_data(mark->data,mark->data_length);
			break;
		}
	}
	if(ed == NULL) {
		return;
	}

	/* set orientation as standard */
	ee = exif_content_get_entry(ed->ifd[EXIF_IFD_0], EXIF_TAG_ORIENTATION);
	if (ee != NULL) {
		set_exif_int(ed, ee, 1);
	}
	ee = exif_content_get_entry(ed->ifd[EXIF_IFD_1], EXIF_TAG_ORIENTATION);
	if (ee != NULL) {
		set_exif_int(ed, ee, 1);
	}
	/* rotate the thumbnail */
	if (ed->data && ed->data[0] == 0xff && ed->data[1] == 0xd8) {
		update_exif_thumbnail(ed, transform);
	}
	/* and (guess what) modify dimension tags according to the rotation */
	update_exif_dimension(ed, transform, srcinfo->image_width, srcinfo->image_height);

	exif_data_save_data(ed, &data, &size);
	exif_data_unref(ed);
	/* update jpeg APP1 (EXIF) marker */
	mark->data = srcinfo->mem->alloc_large((j_common_ptr)srcinfo, JPOOL_IMAGE, size);
	mark->original_length = size;
	mark->data_length = size;
	memcpy(mark->data,data,size);
	free(data);
}

static void jpegtran_do_transform(struct jpeg_decompress_struct *src, struct jpeg_compress_struct *dst, int transform) {
	jvirt_barray_ptr * src_coef_arrays;
	jvirt_barray_ptr * dst_coef_arrays;
        jpeg_transform_info transformoption;

	memset(&transformoption, 0, sizeof(transformoption));
	transformoption.transform = transform;
        transformoption.perfect = FALSE;
        transformoption.trim = FALSE;
        transformoption.force_grayscale = FALSE;
        transformoption.crop = FALSE;

	jcopy_markers_setup(src, JCOPYOPT_ALL);
        jpeg_read_header(src, TRUE);

	transform_exif(src, transform);

	jtransform_request_workspace(src, &transformoption);
	src_coef_arrays = jpeg_read_coefficients(src);
	jpeg_copy_critical_parameters(src, dst);
	dst_coef_arrays = jtransform_adjust_parameters(src, dst, src_coef_arrays, &transformoption);
	jpeg_write_coefficients(dst, dst_coef_arrays);
	jcopy_markers_execute(src, dst, JCOPYOPT_ALL);
	jtransform_execute_transformation(src, dst, src_coef_arrays, &transformoption);

	jpeg_finish_compress(dst);
	jpeg_finish_decompress(src);

	jpeg_destroy_compress(dst);
        jpeg_destroy_decompress(src);
}

void *jpegtransform_threadfunc(void *arg) {
	struct jpeg_info *jpeg_trans_arg = (struct jpeg_info *) arg;
        struct jpeg_decompress_struct src;
        struct jpeg_compress_struct dst;
        struct jpeg_error_mgr jsrcerr, jdsterr;

	src.err = jpeg_std_error(&jsrcerr);
	jpeg_create_decompress(&src);
	jpeg_stdio_src(&src, jpeg_trans_arg->src);

	dst.err = jpeg_std_error(&jdsterr);
	jpeg_create_compress(&dst);
	jpeg_stdio_dest(&dst, jpeg_trans_arg->dest);

	jpegtran_do_transform(&src, &dst, jpeg_trans_arg->transform);

	fclose(jpeg_trans_arg->src);
	fclose(jpeg_trans_arg->dest);

        return NULL;
}

char *get_exif_date(ExifData *ed) {
	if(ed) {
		ExifEntry *entry = exif_data_get_entry(ed, EXIF_TAG_DATE_TIME_ORIGINAL);
		if(entry) {
			char *ret = malloc(entry->size+1);
			ret[entry->size+1] = '\0'; // just in case, to make sure that it's really \0-terminated (robustness)
			memcpy(ret, entry->data, entry->size);
			return ret;
		}
	}
	return NULL;
}

int get_exif_orientation_transform(ExifData *ed) {
	if(ed) {
		ExifByteOrder byte_order = exif_data_get_byte_order(ed);
		ExifEntry *entry = exif_data_get_entry(ed, EXIF_TAG_ORIENTATION);
		if(entry) {
			switch (exif_get_short(entry->data, byte_order)) {
				case 8: { return JXFORM_ROT_270; }
				case 7: { return JXFORM_TRANSVERSE; }
				case 6: { return JXFORM_ROT_90; }
				case 5: { return JXFORM_TRANSPOSE; }
				case 4: { return JXFORM_FLIP_V; }
				case 3: { return JXFORM_ROT_180; }
				case 2: { return JXFORM_FLIP_H; }
				case 1:
				default: { return JXFORM_NONE; }
			}
		}
	}
	return JXFORM_NONE;
}

void *get_jpeg_threadfunc(void *arg) {
	struct jpeg_info *jpeginfo;
	unsigned char buf[128 * 1024];
	int c;
	int fdfrom;
	int fdto;
	pthread_t thread;

	jpeginfo = (struct jpeg_info *) arg;
	fdfrom = jpeginfo->fd_from_gphoto;
	c = read(fdfrom, buf, sizeof(buf));
	if(c > 0) {
		ExifData *ed = exif_data_new_from_data(buf, c);
		int transform = get_exif_orientation_transform(ed);
		char *filename = get_exif_date(ed);
		if(filename != NULL) {
			int i;
			for(i=strlen(filename)-1; i>= 0; i--) {
				if (filename[i] < '0' || filename[i] > '9') {
					filename[i] = '_';
				}
			}
			filename = realloc(filename, strlen(filename) +5);
			strcat(filename, ".jpg");
		} else {
			filename = strdup(jpeginfo->camerafilename);
		}
		char *full_filename = malloc(strlen(receivedir) + strlen(filename) + 2);
		sprintf(full_filename, "%s/%s", receivedir, filename);
		char *localFilename = unique_filename(full_filename);
		free(filename);
		free(full_filename);
		exif_data_unref(ed);

		if(transform == JXFORM_NONE) {
			printf("  Downloading %s to %s ...\n", jpeginfo->camerafilename, localFilename);
			fdto = open(localFilename, O_CREAT | O_WRONLY, 0666);
			if(fdto < 0) {
				fprintf(stderr, "Cannot create file %s\n", localFilename);
			}
		} else {
			printf("  Downloading and JPEG-transforming %s to %s ...\n", jpeginfo->camerafilename, localFilename);
			int fd_fromchecker_tolibjpeg[2];
			pipe(fd_fromchecker_tolibjpeg);
			fdto = fd_fromchecker_tolibjpeg[1];

			jpeginfo->transform = transform;
			jpeginfo->dest = fopen(localFilename, "wb");
			if(jpeginfo->dest != NULL) {
				jpeginfo->src = fdopen(fd_fromchecker_tolibjpeg[0], "rb");
				pthread_attr_t print_thread_attrs;
				pthread_attr_init(&print_thread_attrs);
				pthread_create(&thread, &print_thread_attrs, &jpegtransform_threadfunc, jpeginfo);
				pthread_attr_destroy(&print_thread_attrs);
			} else {
				fprintf(stderr, "Cannot create file %s\n", localFilename);
				close(fd_fromchecker_tolibjpeg[0]);
				close(fd_fromchecker_tolibjpeg[1]);
				fdto = -1;
			}
		}

		if(fdto >= 0) {
			write(fdto, buf, c);
		}
		while(TRUE) {
			c = read(fdfrom, buf, sizeof(buf));
			if(c <= 0) {
				break;
			}
			if(fdto >= 0) {
				write(fdto, buf, c);
			}
		}

		if(fdto >= 0) {
			close(fdto);
		}
		if(transform != JXFORM_NONE) {
			pthread_join(thread, NULL);
		}
		free(localFilename);
		free(jpeginfo->camerafilename);
		free(jpeginfo);
	}	
	close(fdfrom);
	return NULL;
}

void *do_rename_afterwards_threadfunc(void *arg) {
	char *filename = (char *) arg;
	ExifData *ed;
	char *datestr = NULL;

	ed = exif_data_new_from_file(filename);
	if(ed)	{
		datestr = get_exif_date(ed);
		if(datestr != NULL) {
			int i;
			for(i=strlen(datestr)-1; i>= 0; i--) {
				if (datestr[i] < '0' || datestr[i] > '9') {
					datestr[i] = '_';
				}
			}
/*			printf("##Exif Date %s\n", datestr);*/
		} else {
			printf("No date in exif information %s\n", filename);
		}
		exif_data_unref(ed);
	} else {
		libraw = libraw_init(0);
		if (libraw_open_file(libraw, filename) == LIBRAW_SUCCESS) {
			time_t t = libraw->other.timestamp;
			if(t > 0) {
				datestr = malloc(20);
				strftime(datestr, 20, "%Y_%m_%d_%H_%M_%S", localtime(&t));
/*				printf("##Raw Date %s\n", datestr);*/
			}
#if LIBRAW_MINOR_VERSION != 14
			libraw_recycle_datastream(libraw);
#endif
		} else {
			printf("File not readable or no EXIF data / no recognizable libraw data in file %s\n", filename);
		}
		libraw_recycle(libraw);
	}
	if(datestr != NULL && strlen(datestr) > 0) {
		char *newfilename, *ext, *unique;
		int num;
		ext = malloc(strlen(filename)+1);
		ext[0] = '\0';
		newfilename = malloc(strlen(filename)+strlen(datestr)+1); // that should be more than enough
		strcpy(newfilename, filename);
		for(num=strlen(newfilename)-1; num > 0; num--) {
			if(newfilename[num] == '.') {
				strcpy (ext, &newfilename[num]);
				break;
			}
		}
		while(num > 0) {
			if(newfilename[num] == '/') {
				newfilename[num+1] = '\0';
				break;
			}
			num--;
		}
		strcat(newfilename, datestr);
		strcat(newfilename, ext);
		unique = unique_filename(newfilename);
		printf("  Renaming received file %s to %s\n", filename, unique);
		rename(filename, unique);
		free(ext);
		free(newfilename);
		free(unique);
		free(datestr);
	}
	free(filename);
	return NULL;
}

void get_jpeg_file(Camera *camera, GPContext *context, CameraFilePath *path) {
	int retval;
	CameraFile *file;
	int gpipe[2];

	pipe(gpipe);
	retval = gp_file_new_from_fd(&file, gpipe[1]);
	if(retval == GP_OK) {
		struct jpeg_info *jpeginfo = malloc(sizeof(struct jpeg_info));
		jpeginfo->camerafilename = strdup(path->name);
		jpeginfo->fd_from_gphoto = gpipe[0];

		pthread_t thread;
		pthread_attr_t print_thread_attrs;
		pthread_attr_init(&print_thread_attrs);
		pthread_create(&thread, &print_thread_attrs, &get_jpeg_threadfunc, jpeginfo);
		pthread_detach(thread); /* because no need to join this one later */
		pthread_attr_destroy(&print_thread_attrs);

		retval = gp_camera_file_get(camera, path->folder, path->name,
			     GP_FILE_TYPE_NORMAL, file, context);
		if (retval == GP_OK) {
			printf("  Deleting %s on camera...\n", path->name);
			gp_camera_file_delete(camera, path->folder, path->name, context);
			gp_file_free(file);
		}
	} else {
		close(gpipe[1]);
		close(gpipe[0]);
	}
	free(path);
}

void get_any_file(Camera *camera, GPContext *context, CameraFilePath *path) {
	int fd, retval;
	CameraFile *file;
	char *filename;
	char *unique;

	filename = malloc(strlen(path->name)+strlen(receivedir)+2);
	sprintf(filename, "%s/%s", receivedir, path->name);
	unique = unique_filename(filename);

	fd = open(unique, O_CREAT | O_WRONLY, 0666);
	retval = gp_file_new_from_fd(&file, fd);
	if(retval == GP_OK) {
		printf("  Downloading %s from %s to %s ...\n", path->name, path->folder, unique);
		retval = gp_camera_file_get(camera, path->folder, path->name,
			     GP_FILE_TYPE_NORMAL, file, context);
		if (retval == GP_OK) {
			printf("  Deleting %s on camera...\n", path->name);
			gp_camera_file_delete(camera, path->folder, path->name, context);

			/* can do the do_rename_afterwards in separate thread to be back on the USB line asap */
			pthread_t thread;
			pthread_attr_t print_thread_attrs;
			pthread_attr_init(&print_thread_attrs);
			pthread_create(&thread, &print_thread_attrs, &do_rename_afterwards_threadfunc, strdup(unique));
			pthread_detach(thread); /* because no need to join this one later */
			pthread_attr_destroy(&print_thread_attrs);
		}
		gp_file_free(file);
	} else {
		close(fd);
	}
	free(filename);
	free(unique);
	free(path);
}

static void camera_tether(Camera *camera, GPContext *context) {
	TAILQ_HEAD(tailhead, entry) head;
	struct entry {
		CameraFilePath	*cfp;
		TAILQ_ENTRY(entry)	entries;         /* Tail queue. */
	};

	int	retval;
	CameraEventType	evttype;
	CameraFilePath	*path;
	void	*evtdata;
	struct entry *e;

	TAILQ_INIT(&head);                      /* Initialize the queue. */

	printf("Tethering...\n");

	while (1) {
		evtdata = NULL;
		retval = gp_camera_wait_for_event (camera, (head.tqh_first != NULL) ? 0 : 86400000, &evttype, &evtdata, context);
		if (retval != GP_OK)
			break;
		switch (evttype) {
		case GP_EVENT_FILE_ADDED:
			path = (CameraFilePath*)evtdata;
			if(path) {
				CameraFilePath *pathcopy = malloc(sizeof(CameraFilePath));
				memcpy(pathcopy, path, sizeof(CameraFilePath));
				printf("File added on the camera: %s/%s\n", path->folder, path->name);
				if(strcasecmp(&path->name[strlen(path->name) -4], ".jpg") == 0) {
					get_jpeg_file(camera, context, pathcopy);
				} else {
					e = malloc(sizeof(struct entry));      /* Insert at the head. */
					e->cfp = pathcopy;
					TAILQ_INSERT_TAIL(&head, e, entries);
				}
				free(path);
			}
			break;
		case GP_EVENT_FOLDER_ADDED:
			path = (CameraFilePath*)evtdata;
			printf("Folder added on camera: %s / %s\n", path->folder, path->name);
			if(evtdata) {
				free(evtdata);
			}
			break;
		case GP_EVENT_CAPTURE_COMPLETE:
			printf("Capture Complete.\n");
			if(evtdata) {
				free(evtdata);
			}
			break;
		case GP_EVENT_TIMEOUT:
/*			printf("Timeout.\n");*/
			e = head.tqh_first;
			if(e != NULL) {
				get_any_file(camera, context, e->cfp);
				free(e);
				TAILQ_REMOVE(&head, head.tqh_first, entries);
			}
			if(evtdata) {
				free(evtdata);
			}
			break;
		case GP_EVENT_UNKNOWN:
			if (evtdata) {
/*				printf("Unknown event: %s.\n", (char*)evtdata);*/
				free(evtdata);
			} else {
/*				printf("Unknown event.\n");*/
			}
			break;
		default:
/*			printf("Type %d?\n", evttype);*/
			if(evtdata) {
				free(evtdata);
			}
			break;
		}
	}
}

int main(int argc, char **argv) {
	Camera	*camera;
	int	retval;

	if (argc>2) {
		printf("Usage: %s <receivepath>\n", argv[0]);
		exit(1);
	}
	if(argc == 2) {
		receivedir = argv[1];
	} else {
		receivedir = ".";
	}

	GPContext *context = gp_context_new();
	gp_log_add_func(GP_LOG_ERROR, errordumper, NULL);
	gp_camera_new(&camera);

	printf("Camera init.\n");
	do {
		retval = gp_camera_init(camera, context);
		if (retval == GP_OK) {
			break;
		}
		sleep(1);	// retry every second for initial camera
	} while(TRUE);

	do {
		camera_tether(camera, context);

		gp_camera_exit(camera, context);

		do {	// reconnect camera. retry every second
			sleep(1);
			retval = gp_camera_init(camera, context);
		} while (retval != GP_OK);
	} while (TRUE);
	return 0;
}
