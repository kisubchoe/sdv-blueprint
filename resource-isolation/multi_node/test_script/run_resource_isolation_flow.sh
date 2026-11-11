#!/usr/bin/env bash
set -euo pipefail
# Multi-node orchestrator: start flow
# Env overrides: see config.env

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=./config.env
source "${SCRIPT_DIR}/config.env"
# shellcheck source=./lib/utils.sh
source "${SCRIPT_DIR}/lib/utils.sh"

main() {
  require_cmd ssh
  resolve_auth_mode
  info "Auth mode: USE_SSHPASS=$USE_SSHPASS"

  # MASTER_NODE_IP / GUEST_NODE_IP are required (set them in config.env).
  [[ -n "${MASTER_NODE_IP}" ]] || { err "MASTER_NODE_IP is not set. Please set it in config.env."; exit 1; }
  [[ -n "${GUEST_NODE_IP}" ]]  || { err "GUEST_NODE_IP is not set. Please set it in config.env."; exit 1; }
  info "Master node IP: ${MASTER_NODE_IP}"
  info "Guest node IP: ${GUEST_NODE_IP}"

  info "Step 1) master: docker compose up -d"
  on_master "cd '$MASTER_SERIAL_BRIDGE_DIR'; MASTER_IP='$MASTER_NODE_IP' docker compose up -d"

  info "Step 2) master: pullpiri runtime install script"
  on_master_sudo "bash '$MASTER_TEST_SCRIPT_DIR/pullpiri/scripts/install-pullpiri.sh' '' '$GUEST_HOST' '$GUEST_PORT' '$GUEST_USER' '$GUEST_PASS' '$GUEST_SUDO_PASS' '$GUEST_NODE_IP' '$GUEST_TEST_SCRIPT_DIR'"

  info "Step 3) guest: restart nodeagent.service"
  on_guest_sudo "systemctl restart nodeagent.service"

  info "Step 4) guest: start monitoring script (background)"
  on_guest "cd '$GUEST_MONITORING_DIR'; nohup ./start.sh > monitoring_start.log 2>&1 & echo monitoring_started"

  info "Step 5) master: start timpani-o (container)"
  # Load the timpani-o image and run it as a container (see timpani/scripts/install-timpani-o.sh).
  on_master_sudo "bash '$MASTER_TEST_SCRIPT_DIR/timpani/scripts/install-timpani-o.sh'"
  info "Step 5) timpani-o container started on master (logs: sudo podman logs -f timpani-o)"

  info "Allowing guest-to-TIMPANI-O forwarded connections"
  # Keep the rule identical to stop_resource_isolation_flow.sh.
  local timpani_forward_rule
  timpani_forward_rule="-s '${GUEST_NODE_IP}/32' -p tcp --dport 7777 -m conntrack --ctstate NEW,ESTABLISHED \
    --ctorigdst '${MASTER_NODE_IP}' --ctorigdstport 7777 -m conntrack --ctstate DNAT -m comment --comment timpani-managed -j ACCEPT"

  on_master_sudo "
    if check_output=\$(LC_ALL=C iptables -w 5 -C FORWARD ${timpani_forward_rule} 2>&1); then
      echo '[INFO] TIMPANI forwarding rule already exists'
    else
      rc=\$?
      if [ \"\$rc\" -ne 1 ] || [[ -n \"\$check_output\" && \"\$check_output\" != 'iptables: Bad rule (does a matching rule exist in that chain?).' ]]; then
        printf '%s\n' \"\$check_output\" >&2
        echo '[ERROR] Failed to check TIMPANI forwarding rule' >&2
        exit \"\$rc\"
      fi
      echo '[INFO] TIMPANI forwarding rule not found; adding ACCEPT rule'
      iptables -w 5 -I FORWARD 1 ${timpani_forward_rule}
    fi
  "

  echo
  warn "Step 6) After operating the Arduino, proceed with the YAML transmission."
  warn "Watching container '${DATABROKER_CONTAINER}' log on master for up to ${WAIT_LOG_TIMEOUT_SEC}s (regex: ${DATABROKER_LOG_REGEX})"

  set +e
  wait_for_master_container_log
  step6_rc=$?
  set -e

  if [[ $step6_rc -ne 0 ]]; then
    warn "Step 6 log watch failed/timed out (code: $step6_rc)."
    read -r -p "Continue with Step 7? [y/N] " ans
    [[ "$ans" =~ ^[Yy]$ ]] || { err "Aborted by user."; exit 1; }
  fi

  info "Step 7) guest: install & start timpani-n (systemd service)"
  # timpani-n runs on the guest and connects back to the master node IP.
  on_guest_sudo "NODE_NAME=guest NODE_IP='$MASTER_NODE_IP' bash '$GUEST_TEST_SCRIPT_DIR/timpani/scripts/install-timpani-n.sh'"
  info "Step 7) timpani-n service started on guest (logs: sudo journalctl -u timpani-n -f)"

  info "Done."
}

main "$@"
