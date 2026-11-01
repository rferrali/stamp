#include "stamp-contact-view.h"

#include "stamp-account.h"
#include "stamp-book-list.h"
#include "stamp-contact-details.h"
#include "stamp-contact-item.h"
#include "stamp-contact-list.h"

struct _StampContactView {
  AdwBreakpointBin parent_instance;

  AdwMultiLayoutView *contacts_layout;
  AdwNavigationView *mobile_nav;
  AdwOverlaySplitView *tablet_osv;
  AdwOverlaySplitView *mobile_osv;
  GtkPaned *desktop_paned;
  GtkPaned *tablet_paned;

  GtkWidget *book_list;
  GtkWidget *contact_list;
  GtkWidget *contact_details;
  AdwViewStack *stack;

  gint saved_paned_pos;
};

G_DEFINE_FINAL_TYPE (StampContactView, stamp_contact_view, ADW_TYPE_BREAKPOINT_BIN);

static AdwOverlaySplitView *
get_current_osv (StampContactView *self)
{
  const gchar *layout = adw_multi_layout_view_get_layout_name (self->contacts_layout);

  if (g_strcmp0 (layout, "tablet") == 0)
    return self->tablet_osv;
  if (g_strcmp0 (layout, "mobile") == 0)
    return self->mobile_osv;
  return NULL;
}

static void
on_contacts_hidden (AdwNavigationPage *page,
                    gpointer           user_data)
{
  StampContactView *self = STAMP_CONTACT_VIEW (user_data);
  stamp_book_list_unselect (STAMP_BOOK_LIST (self->book_list));
}

static void
on_details_hidden (AdwNavigationPage *page,
                   gpointer           user_data)
{
  StampContactView *self = STAMP_CONTACT_VIEW (user_data);
  stamp_contact_list_unselect (STAMP_CONTACT_LIST (self->contact_list));
}

static void
on_sidebar_visibility_changed (StampContactView *self)
{
  AdwOverlaySplitView *osv = get_current_osv (self);
  GtkWidget *btn;
  gboolean shown;

  if (!osv)
    return;

  shown = adw_overlay_split_view_get_show_sidebar (osv);
  btn = stamp_contact_list_get_sidebar_button (STAMP_CONTACT_LIST (self->contact_list));
  if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (btn)) != shown)
    gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (btn), shown);
}

static void
close_overlay_sidebar (StampContactView *self)
{
  AdwOverlaySplitView *osv = get_current_osv (self);
  if (!osv || !adw_overlay_split_view_get_show_sidebar (osv)) return;
  adw_overlay_split_view_set_show_sidebar (osv, FALSE);
  gtk_toggle_button_set_active (
    GTK_TOGGLE_BUTTON (stamp_contact_list_get_sidebar_button (STAMP_CONTACT_LIST (self->contact_list))), FALSE);
}

static void
on_book_selected (GtkWidget    *object,
                  StampAccount *account,
                  EClient      *client,
                  gpointer      user_data)
{
  StampContactView *self = STAMP_CONTACT_VIEW (user_data);

  close_overlay_sidebar (self);
  stamp_contact_list_load (STAMP_CONTACT_LIST (self->contact_list), client, account);
}

static void
on_contact_selected (GtkWidget        *object,
                     StampAccount     *account,
                     StampContactItem *item,
                     gpointer          user_data)
{
  StampContactView *self = STAMP_CONTACT_VIEW (user_data);

  if (item) {
    const gchar *layout = adw_multi_layout_view_get_layout_name (self->contacts_layout);

    if (g_strcmp0 (layout, "mobile") == 0 && self->mobile_nav) {
      adw_navigation_view_push_by_tag (self->mobile_nav, "content");
    }
  }

  stamp_contact_details_show (STAMP_CONTACT_DETAILS (self->contact_details), account,
                              stamp_contact_list_get_client (STAMP_CONTACT_LIST (self->contact_list)), item);
}

static void
stamp_contact_view_dispose (GObject *object)
{
  /* StampContactView *self = STAMP_CONTACT_VIEW (object); */

  /* gtk_widget_dispose_template (GTK_WIDGET (self), STAMP_TYPE_CONTACT_VIEW); */

  G_OBJECT_CLASS (stamp_contact_view_parent_class)->dispose (object);
}

static void
stamp_contact_view_get_property (GObject    *object,
                                 guint       id,
                                 GValue     *value,
                                 GParamSpec *ps)
{
  if (id == 1)
    g_value_set_object (value, STAMP_CONTACT_VIEW (object)->stack);
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, ps);
}

static void
stamp_contact_view_set_property (GObject      *object,
                                 guint         id,
                                 const GValue *value,
                                 GParamSpec   *ps)
{
  if (id == 1)
    g_set_object (&STAMP_CONTACT_VIEW (object)->stack, g_value_get_object (value));
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID (object, id, ps);
}

static gboolean
is_mobile_view (StampContactView *self)
{
  return g_strcmp0 (adw_multi_layout_view_get_layout_name (self->contacts_layout), "mobile") == 0;
}

static
void
on_apply_view (AdwBreakpoint *breakpoint,
               gpointer       user_data)
{
  StampContactView *self = STAMP_CONTACT_VIEW (user_data);
  gboolean show = is_mobile_view (self);

  stamp_contact_list_set_show_buttons (STAMP_CONTACT_LIST (self->contact_list), show);
  stamp_contact_list_set_key_capture_widget (STAMP_CONTACT_LIST (self->contact_list), !show);
}

void
stamp_contact_view_class_init (StampContactViewClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  object_class->set_property = stamp_contact_view_set_property;
  object_class->get_property = stamp_contact_view_get_property;
  object_class->dispose = stamp_contact_view_dispose;

  g_object_class_install_property (object_class, 1, g_param_spec_object ("stack", NULL, NULL,
                                                                         ADW_TYPE_VIEW_STACK,
                                                                         G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gtk_widget_class_set_template_from_resource (widget_class, "/org/tabos/stamp/views/contact/stamp-contact-view.ui");

  gtk_widget_class_bind_template_child (widget_class, StampContactView, contacts_layout);
  gtk_widget_class_bind_template_child (widget_class, StampContactView, mobile_nav);
  gtk_widget_class_bind_template_child (widget_class, StampContactView, contact_list);
  gtk_widget_class_bind_template_child (widget_class, StampContactView, contact_details);
  gtk_widget_class_bind_template_child (widget_class, StampContactView, book_list);
  gtk_widget_class_bind_template_child (widget_class, StampContactView, tablet_osv);
  gtk_widget_class_bind_template_child (widget_class, StampContactView, mobile_osv);
  gtk_widget_class_bind_template_child (widget_class, StampContactView, desktop_paned);
  gtk_widget_class_bind_template_child (widget_class, StampContactView, tablet_paned);

  gtk_widget_class_bind_template_callback (widget_class, on_book_selected);
  gtk_widget_class_bind_template_callback (widget_class, on_contact_selected);
  gtk_widget_class_bind_template_callback (widget_class, on_contacts_hidden);
  gtk_widget_class_bind_template_callback (widget_class, on_details_hidden);
  gtk_widget_class_bind_template_callback (widget_class, on_apply_view);
}

static void
on_toggle_sidebar (GtkToggleButton *btn G_GNUC_UNUSED,
                   gpointer             user_data)
{
  StampContactView *self = STAMP_CONTACT_VIEW (user_data);
  AdwOverlaySplitView *osv = get_current_osv (self);

  if (!osv)
    return;

  stamp_book_list_unselect (STAMP_BOOK_LIST (self->book_list));
  adw_overlay_split_view_set_show_sidebar (osv, !adw_overlay_split_view_get_show_sidebar (osv));
}

static void
on_layout_changed (AdwMultiLayoutView *view,
                   GParamSpec *ps      G_GNUC_UNUSED,
                   gpointer            user_data)
{
  StampContactView *self = STAMP_CONTACT_VIEW (user_data);
  const gchar *name = adw_multi_layout_view_get_layout_name (view);
  gboolean narrow = g_strcmp0 (name, "desktop") != 0;
  GtkWidget *btn = stamp_contact_list_get_sidebar_button (STAMP_CONTACT_LIST (self->contact_list));

  gtk_widget_set_visible (btn, narrow);

  if (!narrow && self->saved_paned_pos > 50) {
    gtk_paned_set_position (self->desktop_paned, self->saved_paned_pos);
    gtk_paned_set_position (self->tablet_paned, self->saved_paned_pos);
  }
}

static void
on_paned_changed (GtkPaned      *paned,
                  GParamSpec *ps G_GNUC_UNUSED,
                  gpointer       user_data)
{
  StampContactView *self = STAMP_CONTACT_VIEW (user_data);
  GtkPaned *other;
  gint pos = gtk_paned_get_position (paned);

  if (pos < 50)
    return;

  self->saved_paned_pos = pos;
  other = (paned == self->desktop_paned) ? self->tablet_paned : self->desktop_paned;
  if (gtk_paned_get_position (other) != pos)
    gtk_paned_set_position (other, pos);
}

void
stamp_contact_view_init (StampContactView *self)
{
  gtk_widget_init_template (GTK_WIDGET (self));

  adw_multi_layout_view_set_layout_name (self->contacts_layout, "desktop");

  self->saved_paned_pos = 360;

  g_signal_connect (self->contacts_layout, "notify::layout-name", G_CALLBACK (on_layout_changed), self);
  g_signal_connect (stamp_contact_list_get_sidebar_button (STAMP_CONTACT_LIST (self->contact_list)), "clicked", G_CALLBACK (on_toggle_sidebar), self);

  g_signal_connect (self->desktop_paned, "notify::position", G_CALLBACK (on_paned_changed), self);
  g_signal_connect (self->tablet_paned, "notify::position", G_CALLBACK (on_paned_changed), self);

  /* Synchronize button state with show-sidebar */
  g_signal_connect_swapped (self->tablet_osv, "notify::show-sidebar", G_CALLBACK (on_sidebar_visibility_changed), self);
  g_signal_connect_swapped (self->mobile_osv, "notify::show-sidebar", G_CALLBACK (on_sidebar_visibility_changed), self);
}

GtkWidget *
stamp_contact_view_new (void)
{
  return g_object_new (STAMP_TYPE_CONTACT_VIEW, NULL);
}

void
stamp_contact_view_show_contact (StampContactView *self,
                                 const gchar      *mail)
{
  const gchar *layout = adw_multi_layout_view_get_layout_name (self->contacts_layout);

  stamp_contact_list_search_contact (STAMP_CONTACT_LIST (self->contact_list), mail);

  if (g_strcmp0 (layout, "mobile") == 0 && self->mobile_nav)
    adw_navigation_view_pop_to_tag (self->mobile_nav, "main");
}
