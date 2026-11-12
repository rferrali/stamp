#include "stamp-calendar-view.h"

#include <core/gcal-context.h>
#include <core/gcal-event.h>
#include <core/gcal-global.h>
#include <core/gcal-manager.h>
#include <core/gcal-timeline.h>
#include <gcal-timeline-subscriber.h>
#include <glib/gi18n.h>
#include <gui/common/gcal-date-chooser.h>
#include <gui/event-editor/gcal-event-editor-dialog.h>
#include <gui/views/gcal-agenda-view.h>
#include <gui/views/gcal-week-view.h>
#include <utils/gcal-date-time-utils.h>

#include "stamp-account.h"
#include "stamp-calendar-autocleanups.h"
#include "stamp-calendar-list.h"
#include "stamp-category.h"
#include "stamp-session.h"

struct _StampCalendarView {
  GtkWidget parent_instance;

  AdwMultiLayoutView *calendar_layout;
  AdwOverlaySplitView *tablet_osv;
  AdwOverlaySplitView *mobile_osv;
  AdwNavigationView *mobile_nav;
  AdwViewStack *stack;
  GtkToggleButton *sidebar_button;
  GtkWidget *navigation_button;

  GcalAgendaView *agenda;
  GcalWeekView *week;
  GcalDateChooser *date_chooser;
  StampCalendarList *calendar_list;

  StampAccount *account;
  GtkCssProvider *colors_provider;
  GDateTime *active_date;
  GcalEventWidget *preview_event;
  guint preview_timeout;
  gdouble preview_x;
};

typedef enum {
  PROP_STACK = 1,
  PROP_ACTIVE_DATE,
} StampCalendarViewProps;

static GParamSpec *properties[PROP_ACTIVE_DATE + 1] = { NULL, };

G_DEFINE_FINAL_TYPE (StampCalendarView, stamp_calendar_view, ADW_TYPE_BREAKPOINT_BIN);

static void
stamp_calendar_view_get_property (GObject    *object,
                                  guint       id,
                                  GValue     *value,
                                  GParamSpec *ps)
{
  StampCalendarView *self = STAMP_CALENDAR_VIEW (object);

  switch ((StampCalendarViewProps)id) {
    case PROP_STACK:
      g_value_set_object (value, STAMP_CALENDAR_VIEW (object)->stack);
      break;
    case PROP_ACTIVE_DATE:
      g_value_set_boxed (value, self->active_date);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, ps);
  }
}

static void
stamp_calendar_view_set_property (GObject      *object,
                                  guint         id,
                                  const GValue *value,
                                  GParamSpec   *ps)
{
  StampCalendarView *self = STAMP_CALENDAR_VIEW (object);

  switch ((StampCalendarViewProps)id) {
    case PROP_STACK:
      g_set_object (&STAMP_CALENDAR_VIEW (object)->stack, g_value_get_object (value));
      break;
    case PROP_ACTIVE_DATE:
      g_set_date_time (&self->active_date, g_value_get_boxed (value));
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, ps);
      break;
  }
}

static void
remove_event_cb (GcalEventEditorDialog *dialog,
                 GcalEvent             *event,
                 GcalRecurrenceModType  modifier,
                 gpointer               user_data)
{
  GcalContext *context = gcal_get_default_context ();

  gcal_manager_remove_event (gcal_context_get_manager (context), event, modifier);
}

static void
open_event_editor (StampCalendarView *self,
                   GcalEventWidget   *event_widget)
{
  GtkWidget *editor = gcal_event_editor_dialog_new ();
  GcalEvent *event = gcal_event_widget_get_event (event_widget);

  g_signal_connect (editor, "remove-event", G_CALLBACK (remove_event_cb), NULL);
  gcal_event_editor_dialog_present_event ((GcalEventEditorDialog *)editor,
                                          GTK_WIDGET (self), event, FALSE);
}

static void
event_preview_cb (GcalEventWidget        *event_widget,
                  GcalEventPreviewAction  action,
                  gpointer                user_data)
{
  StampCalendarView *self = STAMP_CALENDAR_VIEW (user_data);

  switch (action){
    case GCAL_EVENT_PREVIEW_ACTION_EDIT:
      open_event_editor (self, event_widget);
      break;

    case GCAL_EVENT_PREVIEW_ACTION_NONE:
    default:
      break;
  }
}

static void
cancel_pending_preview (StampCalendarView *self)
{
  g_clear_handle_id (&self->preview_timeout, g_source_remove);
  g_clear_object (&self->preview_event);
}

static void
show_pending_preview (gpointer user_data)
{
  StampCalendarView *self = STAMP_CALENDAR_VIEW (user_data);
  g_autoptr (GcalEventWidget) event_widget = g_steal_pointer (&self->preview_event);

  self->preview_timeout = 0;
  if (gtk_widget_get_root (GTK_WIDGET (event_widget)) == NULL)
    return;

  gcal_event_widget_show_preview (event_widget, event_preview_cb, self);

  if (!gtk_widget_get_ancestor (GTK_WIDGET (event_widget), GTK_TYPE_LIST_BOX) &&
      gcal_event_is_multiday (gcal_event_widget_get_event (event_widget))) {
    GdkRectangle rect = { self->preview_x, 0, 1, 1 };

    for (GtkWidget *child = gtk_widget_get_first_child (GTK_WIDGET (event_widget));
         child != NULL;
         child = gtk_widget_get_next_sibling (child)) {
      if (GTK_IS_POPOVER (child))
        gtk_popover_set_pointing_to (GTK_POPOVER (child), &rect);
    }
  }
}

static void
on_event_click_pressed (GtkGestureClick   *gesture,
                        gint               n_press,
                        gdouble            x,
                        gdouble            y,
                        StampCalendarView *self)
{
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  cancel_pending_preview (self);
}

static void
on_event_click_released (GtkGestureClick   *gesture,
                         gint               n_press,
                         gdouble            x,
                         gdouble            y,
                         StampCalendarView *self)
{
  GcalEventWidget *event_widget = GCAL_EVENT_WIDGET (
    gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture)));
  gint double_click_time;

  cancel_pending_preview (self);
  if (n_press == 2) {
    open_event_editor (self, event_widget);
  } else if (n_press == 1) {
    /* A modal preview would swallow the second click of a double-click. */
    g_object_get (gtk_widget_get_settings (GTK_WIDGET (self)),
                  "gtk-double-click-time", &double_click_time, NULL);
    self->preview_event = g_object_ref (event_widget);
    self->preview_x = x;
    self->preview_timeout = g_timeout_add_once (double_click_time, show_pending_preview, self);
  }
}

static void
on_calendar_click_pressed (GtkGestureClick   *gesture,
                           gint               n_press,
                           gdouble            x,
                           gdouble            y,
                           StampCalendarView *self)
{
  GtkWidget *target = gtk_widget_pick (GTK_WIDGET (self), x, y, GTK_PICK_DEFAULT);
  GtkWidget *event_widget;
  g_autoptr (GListModel) controllers = NULL;
  GtkGesture *click;

  cancel_pending_preview (self);
  if (target == NULL)
    return;

  event_widget = gtk_widget_get_ancestor (target, GCAL_TYPE_EVENT_WIDGET);
  if (event_widget == NULL || g_object_get_data (G_OBJECT (event_widget), "stamp-event-click"))
    return;

  /* Replace only libgcal's pointer click handler. Its release handler requires
   * an immediate preview; keeping the drag controller preserves drag and drop. */
  controllers = gtk_widget_observe_controllers (event_widget);
  for (guint i = 0; i < g_list_model_get_n_items (controllers); i++) {
    g_autoptr (GtkEventController) controller = g_list_model_get_item (controllers, i);

    if (GTK_IS_GESTURE_CLICK (controller))
      gtk_event_controller_set_propagation_phase (controller, GTK_PHASE_NONE);
  }

  click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  g_signal_connect_object (click, "pressed", G_CALLBACK (on_event_click_pressed), self, G_CONNECT_DEFAULT);
  g_signal_connect_object (click, "released", G_CALLBACK (on_event_click_released), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller (event_widget, GTK_EVENT_CONTROLLER (click));
  g_object_set_data (G_OBJECT (event_widget), "stamp-event-click", click);
}

static void
stamp_calendar_view_dispose (GObject *object)
{
  cancel_pending_preview (STAMP_CALENDAR_VIEW (object));

  G_OBJECT_CLASS (stamp_calendar_view_parent_class)->dispose (object);
}


static void
create_meeting (GtkButton         *button,
                StampCalendarView *self)
{
  GcalManager *manager = gcal_context_get_manager (gcal_get_default_context ());
  GcalCalendar *calendar = gcal_manager_get_default_calendar (manager);
  g_autoptr (ECalComponent) component = NULL;
  g_autoptr (GDateTime) now = g_date_time_new_now_local ();
  g_autoptr (GDateTime) start = NULL;
  g_autoptr (GDateTime) end = NULL;
  g_autoptr (ICalTime) start_time = NULL;
  g_autoptr (ICalTime) end_time = NULL;
  g_autoptr (GcalEvent) event = NULL;
  g_autoptr (GError) error = NULL;
  g_autoptr (ECalComponentDateTime) date_time = NULL;
  GtkWidget *editor;

  cancel_pending_preview (self);

  if (calendar == NULL || gcal_calendar_is_read_only (calendar)) {
    g_autoptr (GList) calendars = gcal_manager_get_calendars (manager);

    calendar = NULL;
    for (GList *l = calendars; l; l = l->next) {
      if (!gcal_calendar_is_read_only (l->data)) {
        calendar = l->data;
        break;
      }
    }
  }

  if (calendar == NULL) {
    AdwDialog *dialog = adw_alert_dialog_new (_("No Writable Calendar"),
                                              _("Add a writable calendar to create a meeting."));

    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "close", _("_Close"));
    adw_dialog_present (dialog, GTK_WIDGET (self));
    return;
  }

  start = g_date_time_new_local (g_date_time_get_year (self->active_date),
                                 g_date_time_get_month (self->active_date),
                                 g_date_time_get_day_of_month (self->active_date),
                                 g_date_time_get_hour (now), 0, 0);
  end = g_date_time_add_hours (start, 1);
  start_time = i_cal_time_new_from_timet_with_zone (g_date_time_to_unix (start), FALSE,
                                                    i_cal_timezone_get_utc_timezone ());
  end_time = i_cal_time_new_from_timet_with_zone (g_date_time_to_unix (end), FALSE,
                                                  i_cal_timezone_get_utc_timezone ());
  component = e_cal_component_new_vtype (E_CAL_COMPONENT_EVENT);
  date_time = e_cal_component_datetime_new (start_time, "UTC");
  e_cal_component_set_dtstart (component, date_time);
  e_cal_component_datetime_set_value (date_time, end_time);
  e_cal_component_set_dtend (component, date_time);
  e_cal_component_commit_sequence (component);
  event = gcal_event_new (calendar, component, &error);

  if (event == NULL) {
    AdwDialog *dialog = adw_alert_dialog_new (_("Could Not Create Meeting"), error->message);

    adw_alert_dialog_add_response (ADW_ALERT_DIALOG (dialog), "close", _("_Close"));
    adw_dialog_present (dialog, GTK_WIDGET (self));
    return;
  }

  editor = gcal_event_editor_dialog_new ();
  gcal_event_editor_dialog_present_event ((GcalEventEditorDialog *)editor,
                                          GTK_WIDGET (self), event, TRUE);
}

static void
event_activated (GcalView        *view,
                 GcalEventWidget *event_widget,
                 gpointer         user_data)
{
  StampCalendarView *self = STAMP_CALENDAR_VIEW (user_data);

  cancel_pending_preview (self);
  gcal_event_widget_show_preview (event_widget, event_preview_cb, user_data);
}

void
stamp_calendar_view_class_init (StampCalendarViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->set_property = stamp_calendar_view_set_property;
  object_class->get_property = stamp_calendar_view_get_property;
  object_class->dispose = stamp_calendar_view_dispose;

  properties[PROP_STACK] = g_param_spec_object ("stack", NULL, NULL,
                                                ADW_TYPE_VIEW_STACK,
                                                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  properties[PROP_ACTIVE_DATE] = g_param_spec_boxed ("active-date",
                                                     NULL,
                                                     NULL,
                                                     G_TYPE_DATE_TIME,
                                                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);

  g_object_class_install_properties (object_class, G_N_ELEMENTS (properties), properties);

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/calendar/stamp-calendar-view.ui");

  gtk_widget_class_bind_template_callback (widget_class, event_activated);
  gtk_widget_class_bind_template_callback (widget_class, create_meeting);

  gtk_widget_class_bind_template_child (widget_class, StampCalendarView, agenda);
  gtk_widget_class_bind_template_child (widget_class, StampCalendarView, week);
  gtk_widget_class_bind_template_child (widget_class, StampCalendarView, date_chooser);
  gtk_widget_class_bind_template_child (widget_class, StampCalendarView, calendar_list);
  gtk_widget_class_bind_template_child (widget_class, StampCalendarView, calendar_layout);
  gtk_widget_class_bind_template_child (widget_class, StampCalendarView, sidebar_button);
  gtk_widget_class_bind_template_child (widget_class, StampCalendarView, tablet_osv);
  gtk_widget_class_bind_template_child (widget_class, StampCalendarView, mobile_osv);
}

static void
load_css_providers (StampCalendarView *self)
{
  static gsize lib_resources_loaded = 0;
  GdkDisplay *display;

  display = gtk_widget_get_display (GTK_WIDGET (self));

  /* Icons and styles are shipped by libgcal; Stamp only overrides details. */
  if (g_once_init_enter (&lib_resources_loaded)) {
    g_autoptr (GtkCssProvider) lib_provider = gtk_css_provider_new ();

    gtk_icon_theme_add_resource_path (gtk_icon_theme_get_for_display (display),
                                      "/org/gnome/calendar/icons");
    gtk_css_provider_load_from_resource (lib_provider, "/org/gnome/calendar/style.css");
    gtk_style_context_add_provider_for_display (display,
                                                GTK_STYLE_PROVIDER (lib_provider),
                                                GTK_STYLE_PROVIDER_PRIORITY_APPLICATION - 1);
    g_once_init_leave (&lib_resources_loaded, 1);
  }

  /* Calendar colors */
  self->colors_provider = gtk_css_provider_new ();
  gtk_style_context_add_provider_for_display (display,
                                              GTK_STYLE_PROVIDER (self->colors_provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
}

/* Events take the color of their first Outlook category, so publish those colors to the calendar library */
static void
append_category_colors (StampCalendarView *self,
                        GString           *css)
{
  g_autoptr (GHashTable) colors = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, (GDestroyNotify)gdk_rgba_free);
  GList *accounts = stamp_session_get_accounts (stamp_session_get_default ());

  for (GList *a = accounts; a; a = a->next) {
    for (GList *c = stamp_account_get_categories (a->data); c; c = c->next) {
      const gchar *name = stamp_category_get_name (c->data);
      const gchar *color_str = stamp_category_get_color (c->data);
      g_autofree gchar *rgba_str = NULL;
      GdkRGBA rgba;

      g_debug ("%s: category '%s' color '%s'", G_STRFUNC, name ? name : "(null)", color_str ? color_str : "(null)");

      if (!name || !color_str || !gdk_rgba_parse (&rgba, color_str))
        continue;

      rgba_str = gdk_rgba_to_string (&rgba);
      g_string_append_printf (css, ".color-%u { --event-bg-color: %s; }\n", g_quark_from_string (rgba_str), rgba_str);
      g_hash_table_replace (colors, g_strdup (name), gdk_rgba_copy (&rgba));
    }
  }

  gcal_event_set_category_colors (colors);
}

static void
recalculate_calendar_colors_css (StampCalendarView *self)
{
  GcalContext *context = gcal_get_default_context ();
  g_autoptr (GString) css_colors = NULL;
  g_autoptr (GList) calendars = NULL;
  GcalManager *manager;

  css_colors = g_string_new (NULL);
  manager = gcal_context_get_manager (context);
  calendars = gcal_manager_get_calendars (manager);
  for (GList *l = calendars; l; l = l->next){
    g_autofree gchar *color_str = NULL;
    const GdkRGBA *color;
    GcalCalendar *calendar;
    GQuark color_id;

    calendar = GCAL_CALENDAR (l->data);

    color = gcal_calendar_get_color (calendar);
    color_str = gdk_rgba_to_string (color);
    color_id = g_quark_from_string (color_str);

    g_string_append_printf (css_colors, ".color-%u { --event-bg-color: %s; }\n", color_id, color_str);
  }

  append_category_colors (self, css_colors);
  gtk_css_provider_load_from_string (self->colors_provider, css_colors->str);
}

static void
on_categories_changed (StampCalendarView *self)
{
  recalculate_calendar_colors_css (self);
}

static void
watch_account_categories (StampCalendarView *self,
                          StampAccount      *account)
{
  g_signal_connect_object (account, "categories-changed", G_CALLBACK (on_categories_changed), self, G_CONNECT_SWAPPED);
}

static void
on_account_added (StampSession      *session,
                  StampAccount      *account,
                  StampCalendarView *self)
{
  watch_account_categories (self, account);
}

static AdwOverlaySplitView *
get_current_osv (StampCalendarView *self)
{
  const gchar *layout = adw_multi_layout_view_get_layout_name (self->calendar_layout);

  if (g_strcmp0 (layout, "tablet") == 0)
    return self->tablet_osv;

  if (g_strcmp0 (layout, "mobile") == 0)
    return self->mobile_osv;

  return NULL;
}

static void
on_layout_changed (AdwMultiLayoutView *view,
                   GParamSpec *ps      G_GNUC_UNUSED,
                   gpointer            user_data)
{
  StampCalendarView *self = STAMP_CALENDAR_VIEW (user_data);
  const gchar *name = adw_multi_layout_view_get_layout_name (view);
  gboolean narrow = g_strcmp0 (name, "desktop") != 0;
  GtkToggleButton *toggle_button = self->sidebar_button;

  gtk_widget_set_visible (GTK_WIDGET (toggle_button), narrow);
}

static void
on_toggle_sidebar (GtkToggleButton *btn G_GNUC_UNUSED,
                   gpointer             user_data)
{
  StampCalendarView *self = STAMP_CALENDAR_VIEW (user_data);
  AdwOverlaySplitView *osv = get_current_osv (self);

  if (!osv)
    return;

  adw_overlay_split_view_set_show_sidebar (osv, !adw_overlay_split_view_get_show_sidebar (osv));
}

static void
on_sidebar_visibility_changed (StampCalendarView *self)
{
  AdwOverlaySplitView *osv = get_current_osv (self);
  GtkToggleButton *toggle_button;
  gboolean shown;

  if (!osv)
    return;

  shown = adw_overlay_split_view_get_show_sidebar (osv);
  toggle_button = self->sidebar_button;
  if (gtk_toggle_button_get_active (toggle_button) != shown)
    gtk_toggle_button_set_active (toggle_button, shown);
}

void
stamp_calendar_view_init (StampCalendarView *self)
{
  GcalContext *context;
  GcalTimeline *timeline;
  GtkGesture *click;

  g_type_ensure (GCAL_TYPE_DATE_CHOOSER);
  g_type_ensure (STAMP_TYPE_CALENDAR_LIST);

  context = gcal_context_new ();
  gcal_set_default_context (context);

  gcal_context_startup (context);

  load_css_providers (self);
  g_object_connect (gcal_context_get_manager (context),
                    "swapped-object-signal::calendar-added", recalculate_calendar_colors_css, self,
                    "swapped-object-signal::calendar-changed", recalculate_calendar_colors_css, self,
                    "swapped-object-signal::calendar-removed", recalculate_calendar_colors_css, self,
                    NULL);
  {
    StampSession *session = stamp_session_get_default ();

    for (GList *a = stamp_session_get_accounts (session); a; a = a->next)
      watch_account_categories (self, a->data);
    g_signal_connect_object (session, "account-added", G_CALLBACK (on_account_added), self, G_CONNECT_DEFAULT);
  }
  recalculate_calendar_colors_css (self);

  /* Initialize the shared date before the template synchronizes its bindings. */
  self->active_date = g_date_time_new_now_local ();
  gtk_widget_init_template (GTK_WIDGET (self));
  stamp_calendar_list_set_model (self->calendar_list,
                                 gcal_manager_get_calendars_model (gcal_context_get_manager (context)));

  click = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (click), GDK_BUTTON_PRIMARY);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (click), GTK_PHASE_CAPTURE);
  g_signal_connect_object (click, "pressed", G_CALLBACK (on_calendar_click_pressed), self, G_CONNECT_DEFAULT);
  gtk_widget_add_controller (GTK_WIDGET (self), GTK_EVENT_CONTROLLER (click));

  timeline = gcal_manager_get_timeline (gcal_context_get_manager (context));
  gcal_timeline_add_subscriber (timeline, GCAL_TIMELINE_SUBSCRIBER (self->agenda));
  gcal_timeline_add_subscriber (timeline, GCAL_TIMELINE_SUBSCRIBER (self->week));
  gcal_timeline_add_subscriber (timeline, GCAL_TIMELINE_SUBSCRIBER (self->date_chooser));

  gcal_view_clear_marks (GCAL_VIEW (self->week));

  g_signal_connect (self->calendar_layout, "notify::layout-name", G_CALLBACK (on_layout_changed), self);
  g_signal_connect (self->sidebar_button, "clicked", G_CALLBACK (on_toggle_sidebar), self);

  /* Synchronize button state with show-sidebar */
  g_signal_connect_swapped (self->tablet_osv, "notify::show-sidebar", G_CALLBACK (on_sidebar_visibility_changed), self);
  g_signal_connect_swapped (self->mobile_osv, "notify::show-sidebar", G_CALLBACK (on_sidebar_visibility_changed), self);
}

GtkWidget *
stamp_calendar_view_new (void)
{
  return g_object_new (STAMP_TYPE_CALENDAR_VIEW, NULL);
}
