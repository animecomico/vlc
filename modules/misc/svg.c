/*****************************************************************************
 * svg.c : Put SVG on the video
 *****************************************************************************
 * Copyright (C) 2002, 2003 VideoLAN
 * $Id: svg.c,v 1.2 2003/07/23 17:26:56 oaubert Exp $
 *
 * Authors: Olivier Aubert <oaubert@lisi.univ-lyon1.fr>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option ) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>                                      /* malloc( ), free( ) */
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include <vlc/vlc.h>
#include <vlc/vout.h>
#include "osd.h"
#include "vlc_block.h"
#include "vlc_filter.h"

#include <librsvg-2/librsvg/rsvg.h>

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  Create    ( vlc_object_t * );
static void Destroy   ( vlc_object_t * );
static subpicture_t *RenderText( filter_t *, block_t * );

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/

#define TEMPLATE_TEXT N_( "SVG template file" )
#define TEMPLATE_LONGTEXT N_( "Location of a file holding a SVG template for automatic string conversion" )

vlc_module_begin();
 set_capability( "text renderer", 101 );
 add_shortcut( "svg" );
 add_string( "svg-template-file", "", NULL, TEMPLATE_TEXT, TEMPLATE_LONGTEXT, VLC_TRUE );
 set_callbacks( Create, Destroy );
vlc_module_end();

/**
   Describes a SVG string to be displayed on the video
*/
typedef struct subpicture_data_t
{
    int            i_width;
    int            i_height;
    int            i_chroma;
    /** The SVG source associated with this subpicture */
    byte_t        *psz_text;
    /* The rendered SVG, as a GdkPixbuf */
    GdkPixbuf      *p_rendition;
} subpicture_data_t;

static void Render    ( filter_t *, subpicture_t *, subpicture_data_t * );
static byte_t *svg_GetTemplate ();
static void svg_SizeCallback  (int *width, int *height, gpointer data );
static void svg_RenderPicture (filter_t *p_filter,
                               subpicture_data_t *p_string );
static void FreeString( subpicture_data_t * );

/*****************************************************************************
 * filter_sys_t: svg local data
 *****************************************************************************
 * This structure is part of the filter thread descriptor.
 * It describes the svg specific properties of an output thread.
 *****************************************************************************/
struct filter_sys_t
{
    /* The SVG template used to convert strings */
    byte_t        *psz_template;
    /* Default size for rendering. Initialized to the output size. */
    int            i_width;
    int            i_height;
    vlc_mutex_t   *lock;
};

/*****************************************************************************
 * Create: allocates svg video thread output method
 *****************************************************************************
 * This function allocates and initializes a  vout method.
 *****************************************************************************/
static int Create( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;
    filter_sys_t *p_sys;

    /* Allocate structure */
    p_sys = malloc( sizeof( filter_sys_t ) );
    if( !p_sys )
    {
        msg_Err( p_filter, "Out of memory" );
        return VLC_ENOMEM;
    }

    /* Initialize psz_template */
    p_sys->psz_template = svg_GetTemplate( p_this );
    if( !p_sys->psz_template )
    {
        msg_Err( p_filter, "Out of memory" );
        return VLC_ENOMEM;
    }

    p_sys->i_width = p_filter->fmt_out.video.i_width;
    p_sys->i_height = p_filter->fmt_out.video.i_height;

    p_filter->pf_render_string = RenderText;
    p_filter->p_sys = p_sys;

    /* MUST call this before any RSVG funcs */
    g_type_init ();

    return VLC_SUCCESS;
}

static byte_t *svg_GetTemplate( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;
    char *psz_filename;
    char *psz_template;
    FILE *file;

    psz_filename = config_GetPsz( p_filter, "svg-template-file" );
    if( !psz_filename || psz_filename[0] == 0 )
    {
        /* No filename. Use a default value. */
        psz_template = NULL;
    }
    else
    {
        /* Read the template */
        file = fopen( psz_filename, "rt" );
        if( !file )
        {
            msg_Warn( p_this, "SVG template file %s does not exist.", psz_filename );
            psz_template = NULL;
        }
        else
        {
            struct stat s;
            int i_ret;

            i_ret = lstat( psz_filename, &s );
            if( i_ret )
            {
                /* Problem accessing file information. Should not
                   happen as we could open it. */
                psz_template = NULL;
            }
            else
            {
                msg_Dbg( p_this, "Reading %ld bytes from template %s\n", (long)s.st_size, psz_filename );

                psz_template = malloc( s.st_size + 42 );
                if( !psz_template )
                {
                    msg_Err( p_filter, "Out of memory" );
                    return NULL;
                }
                memset( psz_template, 0, s.st_size + 1 );
                fread( psz_template, s.st_size, 1, file );
                fclose( file );
            }
        }
    }
    if( !psz_template )
    {
        /* Either there was no file, or there was an error.
           Use the default value */
        psz_template = strdup( "<?xml version='1.0' encoding='UTF-8' standalone='no'?> \
<svg version='1' preserveAspectRatio='xMinYMin meet' viewBox='0 0 800 600'> \
  <text x='10' y='560' fill='white' font-size='32'  \
        font-family='sans-serif'>%s</text></svg>" );
    }

    return psz_template;
}

/*****************************************************************************
 * Destroy: destroy Clone video thread output method
 *****************************************************************************
 * Clean up all data and library connections
 *****************************************************************************/
static void Destroy( vlc_object_t *p_this )
{
    filter_t *p_filter = (filter_t *)p_this;
    filter_sys_t *p_sys = p_filter->p_sys;

    free( p_sys->psz_template );
    free( p_sys );
}

/*****************************************************************************
 * Render: render SVG in picture
 *****************************************************************************/
static void Render( filter_t *p_filter, subpicture_t *p_spu,
                    subpicture_data_t *p_string )
{
    int i_width, i_height;
    video_format_t fmt;
    uint8_t *p_y, *p_u, *p_v, *p_a;
    int x, y, i_pitch, i_u_pitch;
    guchar *pixels_in = NULL;
    int rowstride_in;
    int channels_in;
    int alpha;
    picture_t *p_pic;

    if( p_string->p_rendition == NULL ) {
        svg_RenderPicture( p_filter, p_string );
    }
    i_width = gdk_pixbuf_get_width( p_string->p_rendition );
    i_height = gdk_pixbuf_get_height( p_string->p_rendition );

    /* Create a new subpicture region */
    memset( &fmt, 0, sizeof(video_format_t) );
    fmt.i_chroma = VLC_FOURCC('Y','U','V','A');
    fmt.i_aspect = VOUT_ASPECT_FACTOR;
    fmt.i_width = fmt.i_visible_width = i_width;
    fmt.i_height = fmt.i_visible_height = i_height;
    fmt.i_x_offset = fmt.i_y_offset = 0;
    p_spu->p_region = p_spu->pf_create_region( VLC_OBJECT(p_filter), &fmt );
    if( !p_spu->p_region )
    {
        msg_Err( p_filter, "cannot allocate SPU region" );
        return;
    }

    p_spu->p_region->i_x = p_spu->p_region->i_y = 0;
    p_y = p_spu->p_region->picture.Y_PIXELS;
    p_u = p_spu->p_region->picture.U_PIXELS;
    p_v = p_spu->p_region->picture.V_PIXELS;
    p_a = p_spu->p_region->picture.A_PIXELS;

    i_pitch = p_spu->p_region->picture.Y_PITCH;
    i_u_pitch = p_spu->p_region->picture.U_PITCH;

    /* Initialize the region pixels (only the alpha will be changed later) */
    memset( p_y, 0x00, i_pitch * p_spu->p_region->fmt.i_height );
    memset( p_u, 0x80, i_u_pitch * p_spu->p_region->fmt.i_height );
    memset( p_v, 0x80, i_u_pitch * p_spu->p_region->fmt.i_height );

    p_pic = &(p_spu->p_region->picture);

    /* Copy the data */

    /* This rendering code is in no way optimized. If someone has some time to
       make it work faster or better, please do.
    */

    /*
      p_pixbuf->get_rowstride() is the number of bytes in a line.
      p_pixbuf->get_height() is the number of lines.

      The number of bytes of p_pixbuf->p_pixels is get_rowstride * get_height

      if( has_alpha() ) {
      alpha = pixels [ n_channels * ( y*rowstride + x ) + 3 ];
      }
      red   = pixels [ n_channels * ( y*rowstride ) + x ) ];
      green = pixels [ n_channels * ( y*rowstride ) + x ) + 1 ];
      blue  = pixels [ n_channels * ( y*rowstride ) + x ) + 2 ];
    */

    pixels_in = gdk_pixbuf_get_pixels( p_string->p_rendition );
    rowstride_in = gdk_pixbuf_get_rowstride( p_string->p_rendition );
    channels_in = gdk_pixbuf_get_n_channels( p_string->p_rendition );
    alpha = gdk_pixbuf_get_has_alpha( p_string->p_rendition );

    /*
      This crashes the plugin (if !alpha). As there is always an alpha value,
      it does not matter for the moment :

    if( !alpha )
      memset( p_a, 0xFF, i_pitch * p_spu->p_region->fmt.i_height );
    */
 
#define INDEX_IN( x, y ) ( y * rowstride_in + x * channels_in )
#define INDEX_OUT( x, y ) ( y * i_pitch + x * p_pic->p[Y_PLANE].i_pixel_pitch )
#define UV_INDEX_OUT( x, y ) ( ( y >> 1 ) * i_u_pitch + ( x >> 1) * p_pic->p[U_PLANE].i_pixel_pitch )
  
    for( y = 0; y < i_height; y++ )
    {
        for( x = 0; x < i_width; x++ )
        {
            guchar *p_in;
            int i_out;
            int i_uv_out;
 
            p_in = &pixels_in[INDEX_IN( x, y )];
            
#define R( pixel ) *pixel
#define G( pixel ) *( pixel+1 )
#define B( pixel ) *( pixel+2 )
#define ALPHA( pixel ) *( pixel+3 )
 
            /* From http://www.geocrawler.com/archives/3/8263/2001/6/0/6020594/ :
               Y = 0.29900 * R + 0.58700 * G + 0.11400 * B
               U = -0.1687 * r  - 0.3313 * g + 0.5 * b + 128
               V = 0.5   * r - 0.4187 * g - 0.0813 * b + 128
            */
            if ( alpha ) {
                i_out = INDEX_OUT( x, y );
                
                p_pic->Y_PIXELS[i_out] = .299 * R( p_in ) + .587 * G( p_in ) + .114 * B( p_in );
                
		p_pic->A_PIXELS[i_out] = ALPHA( p_in );
                
                if( ( x % 2 == 0 ) && ( y % 2 == 0 ) ) {

                    i_uv_out = UV_INDEX_OUT( x, y );

                    
                    p_pic->U_PIXELS[i_uv_out] = -.1687 * R( p_in ) - .3313 * G( p_in ) + .5 * B( p_in ) + 128;
                    p_pic->V_PIXELS[i_uv_out] = .5 * R( p_in ) - .4187 * G( p_in ) - .0813 * B( p_in ) + 128;
                }
            }
        }
    }
}

static void svg_SizeCallback( int *width, int *height, gpointer data )
{
    subpicture_data_t *p_string = data;

    *width = p_string->i_width;
    *height = p_string->i_height;
    return;
}

static void svg_RenderPicture( filter_t *p_filter,
                               subpicture_data_t *p_string )
{
    /* Render the SVG string p_string->psz_text into a new picture_t
       p_string->p_rendition with dimensions ( ->i_width, ->i_height ) */
    RsvgHandle *p_handle;
    GError *error = NULL;

    p_handle = rsvg_handle_new();

    rsvg_handle_set_size_callback( p_handle, svg_SizeCallback, p_string, NULL );

    rsvg_handle_write( p_handle,
                       p_string->psz_text, strlen( p_string->psz_text ) + 1,
                       &error );
    if( error != NULL )
    {
        msg_Err( p_filter, "Error in handle_write: %s\n", error->message );
        return;
    }
    rsvg_handle_close( p_handle, &error );

    p_string->p_rendition = rsvg_handle_get_pixbuf( p_handle );
    rsvg_handle_free( p_handle );
}


static subpicture_t *RenderText( filter_t *p_filter, block_t *p_block )
{
    filter_sys_t *p_sys = p_filter->p_sys;
    subpicture_t *p_subpic = NULL;
    subpicture_data_t *p_string = NULL;
    char *psz_string;

    /* Sanity check */
    if( !p_block ) return NULL;
    psz_string = p_block->p_buffer;
    if( !psz_string || !*psz_string ) return NULL;

    /* Create and initialize a subpicture */
    p_subpic = p_filter->pf_sub_buffer_new( p_filter );
    if( !p_subpic ) return NULL;

    p_subpic->i_start = p_block->i_pts;
    p_subpic->i_stop = p_block->i_pts + p_block->i_length;
    /* Always replace rendered text when another is displayed */
    p_subpic->b_ephemer = VLC_TRUE;
    p_subpic->b_absolute = VLC_FALSE;

//    msg_Dbg( p_filter, "adding string \"%s\" start_date "I64Fd

    /* Create and initialize private data for the subpicture */
    p_string = malloc( sizeof(subpicture_data_t) );
    if( !p_string )
    {
        msg_Err( p_filter, "Out of memory" );
        p_filter->pf_sub_buffer_del( p_filter, p_subpic );
        return NULL;
    }
    /* Check if the data is SVG or pure text. In the latter case,
       convert the text to SVG. FIXME: find a better test */
    if( strstr( psz_string, "<svg" ))
    {
        /* Data is SVG: duplicate */
        p_string->psz_text = strdup( psz_string );
        if( !p_string->psz_text )
        {
            msg_Err( p_filter, "Out of memory" );
            p_filter->pf_sub_buffer_del( p_filter, p_subpic );
            free( p_string );
            return NULL;
        }
    }
    else
    {
        /* Data is text. Convert to SVG */
        int length;
        byte_t* psz_template = p_sys->psz_template;
        length = strlen( psz_string ) + strlen( psz_template ) + 42;
        p_string->psz_text = malloc( length + 1 );
        if( !p_string->psz_text )
        {
            msg_Err( p_filter, "Out of memory" );
            p_filter->pf_sub_buffer_del( p_filter, p_subpic );
            free( p_string );
            return NULL;
        }
        memset( p_string->psz_text, 0, length + 1 );
        snprintf( p_string->psz_text, length, psz_template, psz_string );
    }
    p_string->i_width = p_sys->i_width;
    p_string->i_height = p_sys->i_height;
    p_string->i_chroma = VLC_FOURCC('Y','U','V','A');

    /* Render the SVG.
       The input data is stored in the p_string structure,
       and the function updates the p_rendition attribute. */
    svg_RenderPicture( p_filter, p_string );

    Render( p_filter, p_subpic, p_string );
    FreeString( p_string );
    block_Release( p_block );

    return p_subpic;
}

static void FreeString( subpicture_data_t *p_string )
{
    free( p_string->psz_text );
    free( p_string->p_rendition );
    free( p_string );
}
