# Locate a ROS 2 installation for the build or launcher.
find_ros_setup() {
  local setup_path="$1" preferred_distro="$2" distro
  if [[ -n "${setup_path}" && -f "${setup_path}" ]]; then
    printf '%s\n' "${setup_path}"
    return
  fi
  for distro in "${preferred_distro}" jazzy lyrical kilted humble; do
    if [[ -n "${distro}" && -f "/opt/ros/${distro}/setup.bash" ]]; then
      printf '/opt/ros/%s/setup.bash\n' "${distro}"
      return
    fi
  done
  return 1
}
