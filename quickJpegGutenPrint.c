#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <libgen.h>
#include <jpeglib.h>
#include <gutenprint/image.h>
#include <gutenprint/printers.h>
#include <gutenprint/util.h>
#include <gutenprint/channel.h>
#include <cups/cups.h>
#include <cups/ppd.h>
#include <cups/raster.h>
#include "transupp/transupp.h"

struct quick_jpeg_print_info {
	struct jpeg_decompress_struct *cinfo;
	JSAMPROW row_pointer[1];
	struct jpeg_error_mgr jerr;
	FILE *infile;	/* original file */
	boolean using_thread;
	pthread_t jpeg_turn_thread;
	int threadpipe[2];
	FILE *pipefile[2];
	struct jpeg_decompress_struct *pipesrcinfo;
};

unsigned int verbose = 1;

/* libjpeg interfacing stuff */

struct quick_jpeg_print_info *start_jpeg_file(const char *filename) {
	struct quick_jpeg_print_info *qinfo = malloc(sizeof(struct quick_jpeg_print_info));
	qinfo->cinfo = malloc(sizeof(struct jpeg_decompress_struct));
	qinfo->using_thread = FALSE;
	qinfo->infile = fopen(filename, "rb");
	if (!qinfo->infile) {
		return NULL;
	}
	qinfo->cinfo->err = jpeg_std_error(&(qinfo->jerr));
	jpeg_create_decompress(qinfo->cinfo);
	jpeg_stdio_src(qinfo->cinfo, qinfo->infile);
	jcopy_markers_setup(qinfo->cinfo, JCOPYOPT_NONE);
	jpeg_read_header(qinfo->cinfo, TRUE);
	return qinfo;
}

void *turn_jpeg_file_threadfunc(void *arg) {
        struct quick_jpeg_print_info *qinfo = (struct quick_jpeg_print_info *) arg;
/* turn like in jpegtran i.e. turn losslessly by turning only the compressed data,
   and should be much more efficient, and even doing it through a pipe for efficiency
   - hah, that's cool, isn't it?
 */
	jpeg_transform_info transformoption;
	jvirt_barray_ptr *src_coef_arrays, *dst_coef_arrays;
	j_decompress_ptr srcinfo;
	struct jpeg_compress_struct dstinfo;

	srcinfo = qinfo->pipesrcinfo;
	transformoption.transform = JXFORM_ROT_270;
	transformoption.perfect = FALSE;
	transformoption.trim = FALSE;
	transformoption.force_grayscale = FALSE;
	transformoption.crop = FALSE;

        dstinfo.err = &(qinfo->jerr);
        jpeg_create_compress(&dstinfo);

	jtransform_request_workspace(srcinfo, &transformoption);
	src_coef_arrays = jpeg_read_coefficients(srcinfo);
	jpeg_copy_critical_parameters(srcinfo, &dstinfo);
	dst_coef_arrays = jtransform_adjust_parameters(srcinfo, &dstinfo, src_coef_arrays, &transformoption);

	jpeg_stdio_dest(&dstinfo, qinfo->pipefile[1]);

	jpeg_write_coefficients(&dstinfo, dst_coef_arrays);
	jtransform_execute_transformation(srcinfo, &dstinfo, src_coef_arrays, &transformoption);
	jpeg_finish_compress(&dstinfo);
	jpeg_destroy_compress(&dstinfo);

	jpeg_finish_decompress(srcinfo);
	jpeg_destroy_decompress(srcinfo);

        fclose(qinfo->pipefile[1]);
        close(qinfo->threadpipe[1]);

	return NULL;
}

void turn_jpeg_file(struct quick_jpeg_print_info *qinfo) {
        if(pipe(qinfo->threadpipe) == 0) {
                qinfo->pipefile[0] = fdopen(qinfo->threadpipe[0], "rb");
                qinfo->pipefile[1] = fdopen(qinfo->threadpipe[1], "wb");
                pthread_attr_t print_thread_attrs;
                if(qinfo->pipefile[0] != NULL && qinfo->pipefile[1] != NULL &&
                   pthread_attr_init(&print_thread_attrs) == 0) {
                        qinfo->using_thread = TRUE;
                        qinfo->pipesrcinfo = qinfo->cinfo;
                        pthread_create(&(qinfo->jpeg_turn_thread), &print_thread_attrs, &turn_jpeg_file_threadfunc, qinfo);
                        pthread_attr_destroy(&print_thread_attrs);

                	qinfo->cinfo = malloc(sizeof(struct jpeg_decompress_struct));
                	jpeg_create_decompress(qinfo->cinfo);
                	qinfo->cinfo->err = &(qinfo->jerr);
                	jpeg_stdio_src(qinfo->cinfo, qinfo->pipefile[0]);
                	jpeg_read_header(qinfo->cinfo, TRUE);
                }
        }
}

void finish_jpeg_file(struct quick_jpeg_print_info *qinfo) {
        if(qinfo->using_thread) {
                pthread_join(qinfo->jpeg_turn_thread, NULL);
                fclose(qinfo->pipefile[0]);
                close(qinfo->threadpipe[0]);
                free(qinfo->pipesrcinfo);
        }
	jpeg_finish_decompress(qinfo->cinfo);
	jpeg_destroy_decompress(qinfo->cinfo);
	fclose(qinfo->infile);
	free(qinfo->cinfo);
	free(qinfo);
}

/* libgutenprint interfacing stuff */

const stp_printer_t *printer;
const struct stp_vars *gvars_defaults;
struct stp_vars *gvars;

void image_init(struct stp_image *image) {
}

void image_reset(struct stp_image *image) {
}

int image_width(struct stp_image *image) {
	return ((struct quick_jpeg_print_info *)image->rep)->cinfo->output_width;
}

int image_height(struct stp_image *image) {
	return ((struct quick_jpeg_print_info *)image->rep)->cinfo->output_height;
}

stp_image_status_t image_get_row(struct stp_image *image, unsigned char *data, size_t byte_limit, int row) {
	struct quick_jpeg_print_info *qinfo = (struct quick_jpeg_print_info *)image->rep;
	if(qinfo->cinfo->output_scanline < qinfo->cinfo->output_height) {
		qinfo->row_pointer[0] = data;
		do { /* This is the main idea for this program:
		        be more efficient by having libjpeg read the image data
		        directly into libgutenprint's buffer,
		        so we can avoid copying around stuff a lot of times */
			jpeg_read_scanlines(qinfo->cinfo, qinfo->row_pointer, 1);
		} while(qinfo->cinfo->output_scanline-1 < row);	/* just in case we should ever see row >1 */
	} else {
		memset(data, 0xff, byte_limit);
	}
	return STP_IMAGE_STATUS_OK;
}

const char* image_get_appname(struct stp_image *image) {
	return "quickJpegGutenPrint";
}

void image_conclude(struct stp_image *image) {
}

stp_image_t *mk_image(struct quick_jpeg_print_info *qinfo) {
	stp_image_t *image = malloc(sizeof(stp_image_t));
	image->init = image_init;
	image->reset = image_reset;
	image->width = image_width;
	image->height = image_height;
	image->get_row = image_get_row;
	image->get_appname = image_get_appname;
	image->conclude = image_conclude;
	image->rep = qinfo;
	return image;
}

void gfunc_print(void *data, const char* buffer, size_t bytes) {
	cupsWriteRequestData(CUPS_HTTP_DEFAULT, buffer, bytes);
}

void gfunc_error(void *data, const char* buffer, size_t bytes) {
	char *txt;
	txt = malloc(bytes +1);
	memcpy(txt, buffer, bytes);
	txt[bytes] = '\0';
	fprintf(stderr, "%s\n", txt);
	free(txt);
}

struct stp_vars *guten_prepare_printer(const stp_printer_t *printer) {
        const struct stp_vars *gvars_defaults;
        struct stp_vars *gvars;

	gvars_defaults = stp_printer_get_defaults(printer);
	gvars = stp_vars_create_copy(gvars_defaults);
	stp_set_outdata(gvars, stdout);
	stp_set_outfunc(gvars, gfunc_print);
	stp_set_errdata(gvars, stderr);
	stp_set_errfunc(gvars, gfunc_error);
	stp_parameter_list_t *list = stp_get_parameter_list(gvars);
	size_t count = stp_parameter_list_count(list);
	int i;
	for(i=0; i<count; i++) {
		const stp_parameter_t *par = stp_parameter_list_param(list, i);
		stp_parameter_t pardesc;
		stp_describe_parameter(gvars, par->name, &pardesc);
		if(pardesc.is_mandatory) {
			switch(pardesc.p_type) {
				case STP_PARAMETER_TYPE_STRING_LIST: {
					stp_set_string_parameter(gvars, pardesc.name, pardesc.deflt.str);
					break;
				}
				case STP_PARAMETER_TYPE_INT: {
					stp_set_int_parameter(gvars, pardesc.name, pardesc.deflt.integer);
					break;
				}
				case STP_PARAMETER_TYPE_BOOLEAN: {
					stp_set_boolean_parameter(gvars, pardesc.name, pardesc.deflt.boolean);
					break;
				}
				case STP_PARAMETER_TYPE_DOUBLE: {
					stp_set_float_parameter(gvars, pardesc.name, pardesc.deflt.dbl);
					break;
				}
				case STP_PARAMETER_TYPE_CURVE: {
					stp_set_curve_parameter(gvars, pardesc.name, pardesc.deflt.curve);
					break;
				}
				case STP_PARAMETER_TYPE_FILE: {
					stp_set_file_parameter(gvars, pardesc.name, pardesc.deflt.str);
					break;
				}
/*				case STP_PARAMETER_TYPE_RAW: {
					stp_set_raw_parameter(gvars, pardesc.name, pardesc.deflt.str);
					break;
				}
*/
				case STP_PARAMETER_TYPE_ARRAY: {
					stp_set_array_parameter(gvars, pardesc.name, pardesc.deflt.array);
					break;
				}
				case STP_PARAMETER_TYPE_DIMENSION: {
					stp_set_dimension_parameter(gvars, pardesc.name, pardesc.deflt.dimension);
					break;
				}
				default: {}
			}
		}
	}
	stp_parameter_list_destroy(list);
	return gvars;
}

void guten_set_user_parameters(struct stp_vars* gvars, char **params, unsigned int params_num) {
	int i;
	for(i=0; i<params_num; i++) {
		stp_parameter_t pardesc;
		char* name = params[2*i];
		char* value = params[2*i +1];
		stp_describe_parameter(gvars, name, &pardesc);
		switch(pardesc.p_type) {
			case STP_PARAMETER_TYPE_STRING_LIST: {
				stp_set_string_parameter(gvars, pardesc.name, value);
				break;
			}
			case STP_PARAMETER_TYPE_INT: {
			        int ival;
                                if(sscanf(value, "%d", &ival)) {
                                        stp_set_int_parameter(gvars, pardesc.name, ival);
                                } else {
                                        printf("Cannot parse gutenprint int parameter %s=%s - skipping\n", name, value);
                                }
                                break;
                        }
                        case STP_PARAMETER_TYPE_BOOLEAN: {
			        boolean bval;
			        boolean correct = FALSE;
			        if(strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0 || strcmp(value, "1") == 0) {
			                bval = TRUE;
			                correct = TRUE;
			        }
			        else if(strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0 || strcmp(value, "0") == 0) {
			                bval = FALSE;
			                correct = TRUE;
			        }
                                if(correct) {
                                        stp_set_boolean_parameter(gvars, pardesc.name, bval);
                                } else {
                                        printf("Cannot parse gutenprint boolean parameter %s=%s - skipping\n", name, value);
                                }
				break;
			}
			case STP_PARAMETER_TYPE_DOUBLE: {
			        double dval;
                                if(sscanf(value, "%lf", &dval)) {
                                        stp_set_float_parameter(gvars, pardesc.name, dval);
                                } else {
                                        printf("Cannot parse gutenprint double parameter %s=%s - skipping\n", name, value);
                                }
				break;
			}
			case STP_PARAMETER_TYPE_CURVE: {
			        printf("Gutenprint curve parameter %s - skipping, freel free to write a curve parser for this tool\n", name);
				break;
			}
			case STP_PARAMETER_TYPE_FILE: {
				stp_set_file_parameter(gvars, pardesc.name, value);
				break;
			}
			case STP_PARAMETER_TYPE_RAW: {
			        printf("Gutenprint raw parameter %s - skipping, freel free to write an implementation to support raw parameter in this tool\n", name);
				break;
			}
			case STP_PARAMETER_TYPE_ARRAY: {
			        printf("Gutenprint array parameter %s - skipping, freel free to write an implementation to support arra parameter in this tool\n", name);
				break;
			}
			case STP_PARAMETER_TYPE_DIMENSION: {
			        int ival;
                                if(sscanf(value, "%d", &ival)) {
                                        stp_set_dimension_parameter(gvars, pardesc.name, ival);
                                } else {
                                        printf("Cannot parse gutenprint dimension parameter %s=%s - skipping\n", name, value);
                                }
                                break;
			}
			default: {
			        printf("Gutenprint parameter %s has unknown type - skippig\n", name);
			}
                }
	}
}

char *guten_parameterclass_desc(stp_parameter_class_t class) {
        switch(class) {
                case STP_PARAMETER_CLASS_FEATURE: { return "Printer feature"; }
                case STP_PARAMETER_CLASS_OUTPUT: { return "Output control"; }
                case STP_PARAMETER_CLASS_CORE: { return "Core Gutenprint"; }
                default: { return "Invalid class"; }
       }
}

/* main stuff - putting the pieces together */

void do_show_cups_options(const char *cupsPrinterName) {
        cups_dest_t *dests;
        const int num = cupsGetDests(&dests);
        int i;
        for(i=0; i<num; i++) {
                if(strcmp(cupsPrinterName, dests[i].name) == 0) {
                        cups_option_t *options = dests[i].options;
                        int numOptions = dests[i].num_options;
                        int j;
                        for(j=0; j<numOptions; j++) {
                                printf("%s = %s\n", options[j].name, options[j].value);
                        }
                        break;
                }
        }
        cupsFreeDests(num, dests);
}

void do_show_gutenprint_parameters(const struct stp_vars *gvars) {
	stp_parameter_list_t *list = stp_get_parameter_list(gvars);
	const size_t count = stp_parameter_list_count(list);
	int i;
	for(i=0; i<count; i++) {
		const stp_parameter_t *par = stp_parameter_list_param(list, i);
		stp_parameter_t pardesc;
		stp_describe_parameter(gvars, par->name, &pardesc);
		printf("%s: ", guten_parameterclass_desc(pardesc.p_class));
		switch(pardesc.p_type) {
			case STP_PARAMETER_TYPE_STRING_LIST: {
				printf("%s (String) %s", pardesc.name, pardesc.deflt.str);
				stp_string_list_t *options = pardesc.bounds.str;
				if(options != NULL) {
					const int count = stp_string_list_count(options);
					if (count > 0) {
						stp_param_string_t *popt = stp_string_list_param(options, 0);
						printf(" out of [%s", popt->name);
						int i;
						for(i=1; i<count; i++) {
							stp_param_string_t *popt = stp_string_list_param(options, i);
							printf(", %s", popt->name);
						}
						printf("]");
					}
				}
				printf("\n");
				break;
			}
			case STP_PARAMETER_TYPE_INT: {
				printf("%s (Int) %d from %d to %d\n", pardesc.name, pardesc.deflt.integer, pardesc.bounds.integer.lower, pardesc.bounds.integer.upper);
				break;
			}
			case STP_PARAMETER_TYPE_BOOLEAN: {
				printf("%s (Boolean) %s\n", pardesc.name, pardesc.deflt.boolean ? "true" : "false");
				break;
			}
			case STP_PARAMETER_TYPE_DOUBLE: {
				printf("%s (Double) %lf from %lf to %lf\n", pardesc.name, pardesc.deflt.dbl, pardesc.bounds.dbl.lower, pardesc.bounds.dbl.upper);
				break;
			}
			case STP_PARAMETER_TYPE_CURVE: {
				printf("%s (Curve)\n", pardesc.name);
				break;
			}
			case STP_PARAMETER_TYPE_FILE: {
				printf("%s (File) %s\n", pardesc.name, pardesc.deflt.str);
				break;
			}
			case STP_PARAMETER_TYPE_RAW: {
				printf("%s (Raw)\n", pardesc.name);
				break;
			}
			case STP_PARAMETER_TYPE_ARRAY: {
				printf("%s (Array)\n", pardesc.name);
				break;
			}
			case STP_PARAMETER_TYPE_DIMENSION: {
				printf("%s (Dimension) %d from %d to %d\n", pardesc.name, pardesc.deflt.dimension, pardesc.bounds.dimension.lower, pardesc.bounds.dimension.upper);
				break;
			}
			default: {}
		}
	}
	stp_parameter_list_destroy(list);
}

void image_orientation_and_area(struct quick_jpeg_print_info *qinfo, struct stp_vars *gvars) {
	int left, right, bottom, top;
	int resolutionX, resolutionY;
	stp_get_imageable_area(gvars, &left, &right, &bottom, &top);
	stp_describe_resolution(gvars, &resolutionX, &resolutionY);

	int printWidthDots = right-left;
	int printHeightDots = bottom-top;
	int printWidthPixels = printWidthDots * resolutionX / 72;
	int printHeightPixels = printHeightDots * resolutionY / 72;
	int imageWidthPixels = qinfo->cinfo->image_width;
	int imageHeightPixels = qinfo->cinfo->image_height;

	if(verbose >= 3) {
        	printf(" print area: (%d, %d) - (%d, %d) typographic dots\n", left, top, right, bottom);
        	printf(" print resolution: %d x %d dots per inch\n", resolutionX, resolutionY);
        	printf(" print area size: %d x %d pixels\n", printWidthPixels, printHeightPixels);
        }
	// orientation?
	if((printWidthDots > printHeightDots && imageWidthPixels < imageHeightPixels) ||(printWidthDots < printHeightDots && imageWidthPixels > imageHeightPixels)) {
	        turn_jpeg_file(qinfo);
	 	int t = imageWidthPixels;
	 	imageWidthPixels = imageHeightPixels;
	 	imageHeightPixels = t;
	}
	// maxpect
	if((double) imageWidthPixels / (double) imageHeightPixels > (double) printWidthDots / (double) printHeightDots) {
	 printHeightDots = printWidthDots * imageHeightPixels / imageWidthPixels;
	 printHeightPixels = printWidthPixels * imageHeightPixels / imageWidthPixels;
	} else {
	 printWidthDots = printHeightDots * imageWidthPixels / imageHeightPixels;
	 printWidthPixels = printHeightPixels * imageWidthPixels / imageHeightPixels;
	}
	// center on print area
	stp_set_left(gvars, left + ((right-left - printWidthDots) / 2));
	stp_set_top(gvars, top + ((bottom-top - printHeightDots) / 2));
	stp_set_width(gvars, printWidthDots);
	stp_set_height(gvars, printHeightDots);

	if(verbose >= 3) {
        	printf(" image full size: %d x %d pixels\n", imageWidthPixels, imageHeightPixels);
        	printf(" image print size: %d x %d pixels\n", printWidthPixels, printHeightPixels);
        }
	int denom = 8;
	int num;
	for(num=1; num<denom; num++) {
	        if((imageWidthPixels * num/denom >= printWidthPixels) && (imageHeightPixels * num/denom >= printHeightPixels)) {
	                break;
	        }
	}
	while(num % 2 == 0) {
	        num /= 2;
	        denom /= 2;
	}
	qinfo->cinfo->scale_num=num;
	qinfo->cinfo->scale_denom=denom;
	jpeg_calc_output_dimensions(qinfo->cinfo);

	if(verbose >= 3) {
        	printf(" image decode coefficient: %d / %d\n", qinfo->cinfo->scale_num, qinfo->cinfo->scale_denom);
        	printf(" image decode size: %d x %d pixels\n", qinfo->cinfo->output_width, qinfo->cinfo->output_height);
        }
	if(qinfo->cinfo->num_components == 1) {
		stp_set_string_parameter(gvars, "InputImageType", "Grayscale");
	} else {
		stp_set_string_parameter(gvars, "InputImageType", "RGB");
	}
}

int main(int argc, char **argv) {
	struct quick_jpeg_print_info *qinfo;
	stp_image_t *image;
	boolean show_usage = FALSE;
	boolean show_cups_options = FALSE;
	boolean show_gutenprint_parameters = FALSE;
	char *cupsPrinterName = NULL;
	char *printer_driver = NULL;
	int cupsNumOptions = 0;
	cups_option_t *cupsOptions = NULL;
	char **gparams = NULL;
	unsigned int gparams_num = 0;
	char **infiles = NULL;
	unsigned int infiles_num = 0;

	stp_init();
	stp_initialize_printer_defaults();

	int n;
	for(n=1; n<argc; n++) {
 	        if(strcmp(argv[n],"?") == 0 || strcasecmp(argv[n],"-h") == 0 || strcasecmp(argv[n],"-help") == 0 || strcasecmp(argv[n],"--help") == 0 ) {
	                show_usage = TRUE;
	                continue;
                }
 	        else if(strcmp(argv[n],"--show-cups-options") == 0) {
	                show_cups_options = TRUE;
	                continue;
                }
 	        else if(strcmp(argv[n],"--show-gutenprint-parameters") == 0) {
	                show_gutenprint_parameters = TRUE;
	                continue;
                }
 	        else if(strcmp(argv[n],"--cups-printer") == 0 || strcmp(argv[n],"-P") == 0) {
 	                n++;
 	                if(n<argc) {
 	                        cupsPrinterName = strdup(argv[n]);
                        } else {
                                show_usage = TRUE;
                        }
	                continue;
                }
 	        else if(strcmp(argv[n],"--gutenprint-driver") == 0 || strcmp(argv[n],"-D") == 0) {
 	                n++;
 	                if(n<argc) {
 	                        printer_driver = strdup(argv[n]);
                        } else {
                                show_usage = TRUE;
                        }
	                continue;
                }
 	        else if(strcmp(argv[n],"--gutenprint-parameter") == 0 || strcmp(argv[n],"-G") == 0) {
 	                n++;
 	                if(n<argc) {
 	                        char *val = strstr(argv[n], "=");
 	                        if(val != NULL) {
 	                                gparams = realloc(gparams, (gparams_num+1)*2*sizeof(char *));
 	                                *val = '\0';
 	                                val++;
 	                                gparams[gparams_num*2] = argv[n];
 	                                gparams[gparams_num*2 +1] = val;
 	                                gparams_num++;
 	                        } else {
 	                                show_usage = TRUE;
 	                        }                              
                        } else {
                                show_usage = TRUE;
                        }
	                continue;
                }
 	        else if(strcmp(argv[n],"--cups-option") == 0 || strcmp(argv[n],"-C") == 0) {
 	                n++;
 	                if(n<argc) {
 	                        char *val = strstr(argv[n], "=");
 	                        if(val != NULL) {
 	                                *val = '\0';
 	                                val++;
 	                                cupsNumOptions = cupsAddOption(argv[n], val, cupsNumOptions, &cupsOptions);
 	                        } else {
 	                                show_usage = TRUE;
 	                        }                              
                        } else {
                                show_usage = TRUE;
                        }
	                continue;
                }
                else if(strcmp(argv[n],"-q") == 0) {
                        verbose = 0;
                }
                else if(strcmp(argv[n],"-v") == 0) {
                        verbose = 2;
                }
                else if(strcmp(argv[n],"-vv") == 0) {
                        verbose = 3;
                }
                else if(strcmp(argv[n],"--") == 0) {
                        if(n < argc-1) {
                                infiles = &argv[n+1];
                                infiles_num = argc-n-1;
                        } else {
                                show_usage = TRUE;
                        }
                        break;
                }
                else if(argv[n][0] == '-') {
                        printf("Unrecognized Option: %s\n", argv[n]);
                        show_usage = TRUE;
                }
                else {
	                infiles = &argv[n];
	                infiles_num = argc-n;
	                break;
	        }
	}

	if(cupsPrinterName == NULL) {
	        cups_dest_t *dests;
	        int num = cupsGetDests(&dests);
	        int i;
	        for(i=0; i<num; i++) {
                        if(dests[i].is_default) {
                                cupsPrinterName = strdup(dests[i].name);
                                if(verbose >= 1) {
                                        printf("Auto-selecting cups printer: %s\n", cupsPrinterName);
                                }
                                break;
                        }
	        }
	        cupsFreeDests(num, dests);
	}
	if(printer_driver == NULL && cupsPrinterName != NULL) {
                const char *ppdName = cupsGetPPD(cupsPrinterName);
                if(ppdName != NULL) {
                        ppd_file_t *ppd = ppdOpenFile(ppdName);
                        if(ppd != NULL) {
                                ppd_attr_t *attr = ppdFindAttr(ppd, "StpDriverName", NULL);
                                if(attr != NULL) {
                                        printer_driver = strdup(attr->value);
                                	if(verbose >= 1) {
                                                printf("Auto-selecting gutenprint driver: %s\n", printer_driver);
                                        }
                                }
                                ppdClose(ppd);
                        }
                }
	}

	if(!show_gutenprint_parameters && !show_cups_options) {
	        if(infiles == NULL) {
	                show_usage = TRUE;
	        }
	        if(cupsPrinterName == NULL) {
	                show_usage = TRUE;
	                printf("No cups printer found/specified.\n");
	        }
	        else if(printer_driver == NULL) {
	                show_usage = TRUE;
	                printf("No gutenprint driver found/specified.\n");
	        }
        }
        if(show_usage) {
		fprintf(stderr, "Usage: %s [-P|--cups-printer PRINTER] [-D|--gutenprint-driver DRIVER] [-C|--cups-option OPTION=VALUE] [-G|--gutenprint-parameter PARAMETER=VALUE] [--show-cups-options] [--show-gutenprint-parameters] [-q|-v|-vv] filename.jpg [...]\n", argv[0]);
		return 1;
	}

	if(show_cups_options) {
		do_show_cups_options(cupsPrinterName);
	}

	printer = stp_get_printer_by_driver(printer_driver);
	if(printer == NULL) {
		fprintf(stderr, "Cannot get gutenprinter driver %s\n", printer_driver);
		return 1;
	}

	gvars = guten_prepare_printer(printer);
	guten_set_user_parameters(gvars, gparams, gparams_num);

	if(show_gutenprint_parameters) {
		do_show_gutenprint_parameters(gvars);
	}

	if(infiles_num > 0) {
        	char *jobname = malloc(1);
        	*jobname = '\0';
        	for(n=0; n<infiles_num; n++) {
	                char *infile = strdup(infiles[n]);
	                char *base = basename(infile);
        	        jobname = realloc(jobname, strlen(jobname)+strlen(infile)+4);
	                if(n > 0) {
	                        strcat(jobname, " / ");
                        }
                        strcat(jobname, base);
	                free(infile);
                }

        	int job_id = cupsCreateJob(CUPS_HTTP_DEFAULT, cupsPrinterName, jobname, cupsNumOptions, cupsOptions);
        	if (job_id <= 0) {
		        fprintf(stderr, "Cannot create cups job on %s\n", cupsPrinterName);
        		return 1;
                }

        	for(n=0; n<infiles_num; n++) {
	                char *infile = infiles[n];

	                if(verbose >= 2) {
	                        printf("File %s\n", infile);
	                }
        	        qinfo = start_jpeg_file(infile);
	                if(qinfo == NULL) {
		                fprintf(stderr, "Cannot read jpeg file %s\n", infile);
		                return 1;
                        }
                        image = mk_image(qinfo);
                        image_orientation_and_area(qinfo, gvars);
                        jpeg_start_decompress(qinfo->cinfo);

                        cupsStartDocument(CUPS_HTTP_DEFAULT, cupsPrinterName, job_id, infile, CUPS_FORMAT_RAW, n == infiles_num-1);
                        if(!stp_start_job(gvars, image)) {
                                fprintf(stderr, "Cannot start gutenprint job");
                                return 1;
                        }

                        stp_print(gvars, image);

                        stp_end_job(gvars, image);
                        cupsFinishDocument(CUPS_HTTP_DEFAULT, cupsPrinterName);
                        finish_jpeg_file(qinfo);
                        free(image);
                }
        	free(jobname);
        }
	stp_vars_destroy(gvars);
	free(printer_driver);
	free(cupsPrinterName);
	return 0;
}
