#!/usr/bin/env bash
# Docker Engine + Compose v2 provisioning for the deployment host. Callers
# source lib/common.sh first for log/warn/err/sudo_if_needed.

provision_host_pkg_mgr() {
  case "$(uname -s)" in
    Darwin) printf 'brew'; return ;;
  esac
  if [ -r /etc/os-release ]; then
    . /etc/os-release
    case "${ID:-},${ID_LIKE:-}" in
      *arch*|*manjaro*|*endeavour*) printf 'pacman'; return ;;
      *debian*|*ubuntu*|*mint*|*pop*) printf 'apt'; return ;;
      *fedora*|*rhel*|*centos*|*rocky*|*alma*) printf 'dnf'; return ;;
      *alpine*) printf 'apk'; return ;;
      *suse*|*opensuse*) printf 'zypper'; return ;;
    esac
  fi
  local candidate
  for candidate in pacman apt-get dnf apk zypper; do
    command -v "$candidate" >/dev/null 2>&1 && { printf '%s' "${candidate%%-*}"; return; }
  done
  printf ''
}

docker_compose_ok() {
  command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1
}

docker_daemon_ok() {
  if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    return 0
  fi
  command -v docker >/dev/null 2>&1 && command -v sudo >/dev/null 2>&1 && \
    sudo docker info >/dev/null 2>&1
}

# Prints the docker invocation with sudo when the current user cannot reach
# the daemon (fresh group membership needs a re-login).
docker_client() {
  if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
    printf 'docker'
  elif command -v sudo >/dev/null 2>&1; then
    printf 'sudo docker'
  else
    printf 'docker'
  fi
}

start_docker_daemon() {
  if [ "$(uname -s)" = "Darwin" ]; then
    if command -v colima >/dev/null 2>&1; then
      colima status >/dev/null 2>&1 || colima start --cpu 4 --memory 6 || true
    fi
    return 0
  fi
  if command -v systemctl >/dev/null 2>&1; then
    sudo_if_needed systemctl enable --now docker >/dev/null 2>&1 || true
  elif command -v rc-service >/dev/null 2>&1; then
    sudo_if_needed rc-update add docker default >/dev/null 2>&1 || true
    sudo_if_needed rc-service docker start >/dev/null 2>&1 || true
  fi
  if [ "$(id -u)" -ne 0 ] && command -v getent >/dev/null 2>&1 && \
     getent group docker >/dev/null 2>&1 && \
     ! id -nG "$USER" 2>/dev/null | grep -qw docker; then
    sudo_if_needed usermod -aG docker "$USER" >/dev/null 2>&1 || true
  fi
}

ensure_docker() {
  local assume_yes="${1:-0}"

  if docker_compose_ok; then
    log "Docker + Compose already available."
  else
    if [ "$assume_yes" -ne 1 ] && [ ! -t 0 ]; then
      warn "Docker/Compose missing; re-run with -y to install them automatically."
      return 1
    fi
    if [ "$assume_yes" -ne 1 ]; then
      printf 'Install Docker and Compose now? [Y/n] '
      local reply=""
      read -r reply </dev/tty || reply=""
      case "$reply" in [nN]*) return 1 ;; esac
    fi

    local mgr
    mgr="$(provision_host_pkg_mgr)"
    log "Installing Docker + Compose via ${mgr:-unknown package manager}..."
    case "$mgr" in
      pacman)
        sudo_if_needed pacman -S --needed --noconfirm docker docker-compose ;;
      apt)
        sudo_if_needed apt-get update -y
        sudo_if_needed apt-get install -y --no-install-recommends docker.io
        sudo_if_needed apt-get install -y --no-install-recommends docker-compose-v2 \
          || sudo_if_needed apt-get install -y --no-install-recommends docker-compose-plugin \
          || sudo_if_needed apt-get install -y --no-install-recommends docker-compose ;;
      dnf)
        sudo_if_needed dnf install -y docker docker-compose \
          || sudo_if_needed dnf install -y moby-engine docker-compose ;;
      apk)
        sudo_if_needed apk add --no-cache docker docker-cli-compose ;;
      zypper)
        sudo_if_needed zypper install -y docker docker-compose ;;
      brew)
        command -v colima >/dev/null 2>&1 || brew install colima
        command -v docker >/dev/null 2>&1 || brew install docker docker-compose ;;
      *)
        warn "Unknown distribution; install Docker Engine + Compose v2 manually."
        return 1 ;;
    esac
  fi

  start_docker_daemon

  if ! docker_compose_ok; then
    warn "Docker Compose v2 is still unavailable."
    return 1
  fi
  if ! docker_daemon_ok; then
    warn "Docker daemon not reachable; log out and back in (or 'newgrp docker')."
    return 1
  fi

  log "Docker: $(docker --version 2>/dev/null | head -1)"
  log "Compose: $(docker compose version --short 2>/dev/null || echo '?')"
}
