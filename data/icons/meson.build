application_id = 'org.tabos.stamp'

scalable_dir = 'hicolor' / 'scalable' / 'apps'
scalable_icon = application_id + (get_option('devel') ? '.Devel' : '')
install_data(
  scalable_dir / ('@0@.svg').format(scalable_icon),
  rename: ('@0@.svg').format(application_id),
  install_dir: get_option('datadir') / 'icons' / scalable_dir
)

symbolic_dir = 'hicolor' / 'symbolic' / 'apps'
install_data(
  symbolic_dir / ('@0@-symbolic.svg').format(application_id),
  install_dir: get_option('datadir') / 'icons' / symbolic_dir
)
