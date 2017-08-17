/**
 * usbbootgui - minimalistic frontend for usbboot
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dirent.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <glib.h>
#include <glib/gprintf.h>
#include <gtk/gtk.h>
#include <libusb-1.0/libusb.h>
#include "config.h"
#include "stealcookie.h"

#ifdef ENABLE_NLS
#include <libintl.h>
#define _(String) gettext(String)
#else
#define _(String) String
#endif

/* Information stored in the tree model: */
#define MODEL_IDX_ICON 	 0 /* Icon of image */
#define MODEL_IDX_NAME   1 /* Friendly name */
#define MODEL_IDX_IMAGE  2 /* Image folder name */

/* Prefix to image folders */
#define IMAGE_PREFIX  "/usr/share/rpiboot"

#define SETTINGS_FILE "/etc/usbbootgui.conf"

/* USB device ids of interest */
#define PI_USB_VID       	0x0a5c
#define PI_USB_PRODUCT_BCM2708  0x2763
#define PI_USB_PRODUCT_BCM2709  0x2764

static GtkBuilder *builder;
static GKeyFile *settings;

/* rpiboot progress dialog */
static GtkWidget *progressDialog = NULL;
static GtkWidget *progressBar = NULL;
static GtkWidget *progressLabel = NULL;
static int progress, filecount;

/* hotplug monitoring (only used when operating as tray icon) */
static libusb_context *usbctx = NULL;
static libusb_hotplug_callback_handle usbcbhandle;

/* Show an error message */
static void displayError(const gchar *message)
{
	GtkWidget *dialog = gtk_message_dialog_new (NULL, 0, GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", message);
	gtk_dialog_run (GTK_DIALOG (dialog));
	gtk_widget_destroy (dialog);
}

/* Prompt user for folder, and returns selected path
 * Caller is responsible for free'ing result*/
static gchar *promptForFolder()
{
	GtkWidget *dialog;
	GtkFileChooserAction action = GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
	gchar *folder = NULL, *bootcodepath;

	dialog = gtk_file_chooser_dialog_new (_("Select folder"), NULL, action, _("_Cancel"),
			GTK_RESPONSE_CANCEL, _("_Open"), GTK_RESPONSE_ACCEPT, NULL);

	if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_ACCEPT)
	{
		GtkFileChooser *chooser = GTK_FILE_CHOOSER (dialog);
		folder = gtk_file_chooser_get_filename ( GTK_FILE_CHOOSER(dialog) );
	}

	gtk_widget_destroy (dialog);
	
	bootcodepath = g_strdup_printf ("%s/bootcode.bin", folder);

	if (!g_file_test (bootcodepath, G_FILE_TEST_EXISTS))
	{
		displayError( _("Specified image folder does not contain bootable files (bootcode.bin missing)"));
		g_free (folder);
		folder = NULL;
	}
	
	return folder;
}

/* Called when rpiboot outputs a status message */
static gboolean onCommandOutput(GIOChannel *channel, GIOCondition cond, gpointer data)
{
	gchar *line;

	if( cond == G_IO_HUP )
	{
		return FALSE;
	}

	if (g_io_channel_read_line(channel, &line, NULL, NULL, NULL) )
	{
		gtk_label_set_text (GTK_LABEL (progressLabel), line);
		//gtk_progress_bar_pulse (GTK_PROGRESS_BAR (progressBar));
		progress++;
		gtk_progress_bar_set_fraction (GTK_PROGRESS_BAR (progressBar), MIN (1.0, progress / (double) filecount));
                g_free (line);
	}
	
	return TRUE;
}

/* Called when rpiboot exits */
static void onCommandExit(GPid pid, gint status, gpointer data)
{
	if (status != 0)
	{
		displayError ( _("Error executing rpiboot"));
	}

	gtk_dialog_response (GTK_DIALOG (progressDialog), GTK_RESPONSE_OK);	
}

static int countFiles(const char *path)
{
	struct dirent *entry;
	int count = 0;
	DIR *dir = opendir (path);
	
	if (!dir)
		return 0;
	
	while ((entry = readdir (dir)) != NULL)
	{
		if (entry->d_type == DT_REG)
			count++;
	}

	closedir (dir);
	
	return count;
}


/* Execute rpiboot to do the hard work */
static gboolean usbboot(gchar *imagepath)
{
	gint fd_out, fd_err;
	GPid pid;
	GIOChannel *ch_out, *ch_err;
	gboolean success = TRUE;
	gchar *cmd[] = {"rpiboot", "-d", imagepath, NULL};

	if (!g_spawn_async_with_pipes (NULL, cmd, NULL, G_SPAWN_DO_NOT_REAP_CHILD | G_SPAWN_SEARCH_PATH, 
			NULL, NULL, &pid, NULL, &fd_out, &fd_err, NULL))
	{
		displayError ( _("Error starting rpiboot"));
		return FALSE;
	}

	ch_out = g_io_channel_unix_new (fd_out);
	ch_err = g_io_channel_unix_new (fd_err);
	g_io_add_watch (ch_out, G_IO_IN | G_IO_HUP, onCommandOutput, NULL);
	g_io_add_watch (ch_err, G_IO_IN | G_IO_HUP, onCommandOutput, NULL);
	g_child_watch_add (pid, onCommandExit, NULL);
	
	/* Use file count as estimate of how many steps progress bar needs to have */
	filecount = countFiles(imagepath) + 7;
	progress = 0;

	progressDialog = gtk_dialog_new_with_buttons (_("Progress"), NULL, GTK_DIALOG_MODAL,
		GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, NULL);
	progressBar = gtk_progress_bar_new ();
        gtk_widget_set_size_request (progressBar, 250, 20);
	progressLabel = gtk_label_new (NULL);
	GtkContainer *ca = GTK_CONTAINER (gtk_dialog_get_content_area (GTK_DIALOG( progressDialog )));
	gtk_container_add (ca, progressBar);
	gtk_container_add (ca, progressLabel);
	gtk_widget_show (progressBar);
	gtk_widget_show (progressLabel);        

	if (gtk_dialog_run (GTK_DIALOG (progressDialog)) != GTK_RESPONSE_OK)
	{
		kill(pid, SIGTERM);
		success = FALSE;
	}

	g_io_channel_unref (ch_out);
	g_io_channel_unref (ch_err);
	g_spawn_close_pid (pid);
	gtk_widget_destroy (progressDialog);
	
	return success;
}

static char *getPiType()
{
	libusb_context *ctx;
	struct libusb_device **devs = NULL;
	struct libusb_device_descriptor desc;
	ssize_t i, count;
	char *pitype = NULL;

	count = libusb_get_device_list (ctx, &devs);
	for (i = 0; i < count; i++)
	{
		if (libusb_get_device_descriptor (devs[i], &desc) == 0)
		{
			if (desc.idVendor == PI_USB_VID && desc.iSerialNumber == 0)
			{
				if (desc.idProduct == PI_USB_PRODUCT_BCM2708)
				{
					pitype = "BCM2708";
					break;
				}
				else if (desc.idProduct == PI_USB_PRODUCT_BCM2709)
				{
					pitype = "BCM2709";
					break;
				}
			}
		}
	}
	
	if (devs)
		libusb_free_device_list(devs, 1);

	return pitype;
}

static char *getAlwaysUseImage()
{
	return g_key_file_get_string (settings, "general", "alwaysUseImage", NULL);
}

static void setAlwaysUseImage(const char *image)
{
	g_key_file_set_string (settings, "general", "alwaysUseImage", image);
	g_key_file_save_to_file (settings, SETTINGS_FILE, NULL);
}

static gboolean hasAlwaysUseImage()
{
	char *i = getAlwaysUseImage();
	gboolean result = i && i[0];
	g_free(i);

	return result;
}

/* Called when the user double-clicks on an item or presses enter */
static void onRowActivated(GtkTreeView  *treeview, GtkTreePath *path, GtkTreeViewColumn  *col, gpointer userdata)
{
	gtk_dialog_response (GTK_DIALOG (userdata), GTK_RESPONSE_OK);
}

static void showDialog()
{
	GtkWidget *dialog;
	GtkTreeView *tree;
	GtkTreeIter iter;
	GtkTreeModel *model;
	GtkTreePath *path;
	GtkLabel *typelabel;
	GtkToggleButton *alwayscheck;
	gchar *image, *imagepath, *bootcodepath, *modelstr;
	gboolean remember = FALSE;
	
	dialog = GTK_WIDGET (gtk_builder_get_object (builder, "maindialog"));
	tree   = GTK_TREE_VIEW (gtk_builder_get_object (builder, "treeview"));
	typelabel = GTK_LABEL (gtk_builder_get_object (builder, "typelabel"));
	alwayscheck = GTK_TOGGLE_BUTTON (gtk_builder_get_object (builder, "alwayscheck"));

	/* Select first row */
	path = gtk_tree_path_new_from_indices (0, -1);
	gtk_tree_view_set_cursor (tree, path, NULL, 0);
	gtk_tree_path_free (path);
	
	/* Detect Pi model and display it in the GUI. */
	modelstr = g_strconcat (gtk_label_get_text(typelabel), getPiType(), NULL);
	gtk_label_set_text (typelabel, modelstr);
	g_free (modelstr);
	
	g_signal_connect(tree, "row-activated", (GCallback) onRowActivated, dialog);

	do
	{
		if (gtk_dialog_run (GTK_DIALOG (dialog)) == GTK_RESPONSE_OK)
		{
			gtk_widget_hide (dialog);

			if (gtk_tree_selection_get_selected (gtk_tree_view_get_selection (tree), &model, &iter))
			{
				gtk_tree_model_get (model, &iter, MODEL_IDX_IMAGE, &image,  -1);

				if (!image || !strlen (image))
				{
					/* Prompt user for path */
					imagepath = promptForFolder();
				}
				else
				{
					imagepath = g_strdup_printf ("%s/%s", IMAGE_PREFIX, image);
				}

				if (imagepath)
				{
					/* Sanity check: check that image folder is present, and contains at least bootcode.bin */
					bootcodepath = g_strdup_printf ("%s/bootcode.bin", imagepath);

					if (!g_file_test (imagepath, G_FILE_TEST_EXISTS))
					{
						displayError( _("Specified image folder does not exist"));
					}
					else if (!g_file_test (bootcodepath, G_FILE_TEST_EXISTS))
					{
						displayError( _("Specified image folder does not contain bootable files (bootcode.bin missing)"));
					}
					else
					{
						remember = gtk_toggle_button_get_active (alwayscheck);
						if (remember)
						{
							if (!image || !strlen (image))
								setAlwaysUseImage(imagepath);
							else
								setAlwaysUseImage(image);
						}
						
						if (!usbboot (imagepath))
							break;
					}

					g_free (bootcodepath);
				}

				g_free (image);
				g_free (imagepath);
			}
		}
		else
		{
			break;
		}

	} while ( getPiType() && !remember ); /* Show dialog another time if there are more Pi */
	
	gtk_widget_destroy (dialog);
}

static void menuItemSelected (GtkWidget *menuitem, gpointer userdata)
{
	char *image = (char *) userdata;
	
	if (!gtk_widget_get_visible (menuitem))
		return;
	
	if (!image || !strlen(image))
	{
		image = promptForFolder();
		if (!image)
			return;
	}
	else if (strcmp(image, "ask") == 0)
	{
		setAlwaysUseImage("");
		gtk_main_quit();
		return;		
	}
	
	setAlwaysUseImage(image);
}

/* Show popup menu if tray icon is clicked */
static void onTrayIconClicked(GtkWidget *treeview, GdkEventButton *event, gpointer userdata)
{
	GtkWidget *item;
	GSList *radiogroup = NULL;
	GtkTreeIter iter;
	gboolean valid;
	GtkTreeModel *store = GTK_TREE_MODEL (gtk_builder_get_object (builder, "liststore"));
	char *selectedImage = getAlwaysUseImage();

	GtkWidget *menu = gtk_menu_new ();
	item = gtk_menu_item_new_with_label (_("Role for next Pi plugged in:"));
	gtk_widget_set_sensitive (item, FALSE);
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	item = gtk_separator_menu_item_new();
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);

	/* Add each image listed in the store to the menu */
	valid = gtk_tree_model_get_iter_first (store, &iter);
	while (valid)
	{
		gchar *name, *image;
		
		gtk_tree_model_get (store, &iter,
				    MODEL_IDX_NAME, &name,
				    MODEL_IDX_IMAGE, &image,
				    -1);

		item = gtk_radio_menu_item_new_with_label (radiogroup, name);
		if (!radiogroup)
			radiogroup = gtk_radio_menu_item_get_group (GTK_RADIO_MENU_ITEM (item));
		if (image && selectedImage && strcmp(image, selectedImage) == 0)
			gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM (item), TRUE);
		if (!image && selectedImage && selectedImage[0] == '/') /* custom image selected */
			gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM (item), TRUE);		
		
		gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
		g_signal_connect(item, "activate", G_CALLBACK (menuItemSelected), image);

		g_free (name);
		valid = gtk_tree_model_iter_next (store, &iter);
	}

	item = gtk_radio_menu_item_new_with_label (radiogroup, _("Ask me each time"));
	if (!selectedImage || !strlen(selectedImage))
		gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM (item), TRUE);
	
	g_signal_connect(item, "activate", G_CALLBACK (menuItemSelected), "ask");
	gtk_menu_shell_append (GTK_MENU_SHELL (menu), item);
	gtk_widget_show_all (menu);
	gtk_menu_popup (GTK_MENU (menu), NULL, NULL, NULL, NULL, event->button, event->time);
	g_free(selectedImage);
}

static gboolean pollForPi()
{
	gboolean success;
	char *image, *imagepath;
	
	while ( getPiType() )
	{
		image = getAlwaysUseImage();
		
		if (!image || !image[0])
			showDialog();
		else
		{
			if (image[0] == '/')
				success = usbboot (image);
			else
			{
				imagepath = g_strdup_printf ("%s/%s", IMAGE_PREFIX, image);
				success = usbboot (imagepath);
				g_free (imagepath);
			}
			
			if (!success)
			{
				/* Quit if cancelled by user */
				gtk_main_quit();
				return FALSE;
			}
		}
		g_free (image);
	}
	
	return TRUE;
}

static void showTrayIcon()
{
	GtkStatusIcon *icon = gtk_status_icon_new_from_file (PACKAGE_DATA_DIR "/raspberry-pi.png");
	g_signal_connect(icon, "button-press-event", G_CALLBACK (onTrayIconClicked), NULL);
	gtk_status_icon_set_visible (icon, TRUE);

	if (pollForPi())
	{
		/* Poll for more Pi every 2 seconds */
		g_timeout_add_seconds (2, pollForPi, NULL);
		gtk_main ();
	}
}

int main(int argc, char *argv[])
{
#ifdef ENABLE_NLS
	bindtextdomain (GETTEXT_PACKAGE, LOCALE_DIR);
	bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
	textdomain (GETTEXT_PACKAGE);
#endif

	if (!getenv ("DISPLAY"))
		stealcookie();

	gtk_init (&argc, &argv);
	libusb_init (&usbctx);
	settings = g_key_file_new ();
	if (access (SETTINGS_FILE, F_OK) == 0)
		g_key_file_load_from_file (settings, SETTINGS_FILE, G_KEY_FILE_KEEP_COMMENTS | G_KEY_FILE_KEEP_TRANSLATIONS, NULL);

	gtk_window_set_default_icon_from_file (PACKAGE_DATA_DIR "/raspberry-pi.png", NULL);	
	builder = gtk_builder_new();
	gtk_builder_add_from_file (builder, PACKAGE_DATA_DIR "/usbbootgui.ui", NULL);

	if ( !hasAlwaysUseImage() )
		showDialog();
	if ( hasAlwaysUseImage() )
		showTrayIcon();

	g_key_file_free (settings);
	libusb_exit (usbctx);
	g_object_unref (builder);

	return 0;
}
