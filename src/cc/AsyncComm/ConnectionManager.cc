/**
 * Copyright (C) 2008 Doug Judd (Zvents, Inc.)
 *
 * This file is part of Hypertable.
 *
 * Hypertable is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * Hypertable is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "Common/Compat.h"

#include <cstdlib>
#include <iostream>

extern "C" {
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
}

#include "Common/Error.h"
#include "Common/Logger.h"
#include "Common/System.h"
#include "Common/Time.h"

#include "AsyncComm/Comm.h"

#include "ConnectionManager.h"

using namespace Hypertable;
using namespace std;

void
ConnectionManager::add(const sockaddr_in &addr, uint32_t timeout_ms,
                       const char *service_name, DispatchHandlerPtr &handler) {
  ScopedLock lock(m_impl->mutex);
  SockAddrMap<ConnectionStatePtr> iter;
  ConnectionState *conn_state;

  if (m_impl->conn_map.find(addr) != m_impl->conn_map.end())
    return;

  conn_state = new ConnectionState();
  conn_state->connected = false;
  conn_state->addr = addr;
  memset(&conn_state->local_addr, 0, sizeof(struct sockaddr_in));
  conn_state->timeout_ms = timeout_ms;
  conn_state->handler = handler;
  conn_state->service_name = (service_name) ? service_name : "";
  boost::xtime_get(&conn_state->next_retry, boost::TIME_UTC);

  m_impl->conn_map[addr] = ConnectionStatePtr(conn_state);

  {
    ScopedLock conn_lock(conn_state->mutex);
    send_connect_request(conn_state);
  }
}


void
ConnectionManager::add(const sockaddr_in &addr, uint32_t timeout_ms,
                       const char *service_name) {
  DispatchHandlerPtr null_disp_handler;
  add(addr, timeout_ms, service_name, null_disp_handler);
}


void
ConnectionManager::add(const sockaddr_in &addr, const sockaddr_in &local_addr,
                       uint32_t timeout_ms, const char *service_name,
                       DispatchHandlerPtr &handler) {
  ScopedLock lock(m_impl->mutex);
  SockAddrMap<ConnectionStatePtr> iter;
  ConnectionState *conn_state;

  if (m_impl->conn_map.find(addr) != m_impl->conn_map.end())
    return;

  conn_state = new ConnectionState();
  conn_state->connected = false;
  conn_state->addr = addr;
  conn_state->local_addr = local_addr;
  conn_state->timeout_ms = timeout_ms;
  conn_state->handler = handler;
  conn_state->service_name = (service_name) ? service_name : "";
  boost::xtime_get(&conn_state->next_retry, boost::TIME_UTC);

  m_impl->conn_map[addr] = ConnectionStatePtr(conn_state);

  {
    ScopedLock conn_lock(conn_state->mutex);
    send_connect_request(conn_state);
  }
}


void
ConnectionManager::add(const sockaddr_in &addr, const sockaddr_in &local_addr,
                       uint32_t timeout_ms, const char *service_name) {
  DispatchHandlerPtr null_disp_handler;
  add(addr, local_addr, timeout_ms, service_name, null_disp_handler);
}


bool
ConnectionManager::wait_for_connection(const sockaddr_in &addr,
                                       uint32_t max_wait_ms) {
  Timer timer(max_wait_ms, true);
  return wait_for_connection(addr, timer);
}


bool
ConnectionManager::wait_for_connection(const sockaddr_in &addr,
                                       Timer &timer) {
  ConnectionStatePtr conn_state_ptr;

  timer.start();

  {
    ScopedLock lock(m_impl->mutex);
    SockAddrMap<ConnectionStatePtr>::iterator iter =
        m_impl->conn_map.find(addr);
    if (iter == m_impl->conn_map.end())
      return false;
    conn_state_ptr = (*iter).second;
  }

  {
    boost::mutex::scoped_lock conn_lock(conn_state_ptr->mutex);
    boost::xtime drop_time;

    while (!conn_state_ptr->connected) {
      boost::xtime_get(&drop_time, boost::TIME_UTC);
      xtime_add_millis(drop_time, timer.remaining());
      if (!conn_state_ptr->cond.timed_wait(conn_lock, drop_time))
        return false;
    }
  }

  return true;
}




/**
 * Attempts to establish a connection for the given ConnectionState object.  If
 * a failure occurs, it prints an error message and then schedules a retry by
 * updating the next_retry member of the conn_state object and pushing it onto
 * the retry heap
 *
 * @param conn_state The connection state record
 */
void
ConnectionManager::send_connect_request(ConnectionState *conn_state) {
  int error;
  DispatchHandlerPtr handler(this);

  if (conn_state->local_addr.sin_port != 0)
    error = m_impl->comm->connect(conn_state->addr, conn_state->local_addr,
                                  handler);
  else
    error = m_impl->comm->connect(conn_state->addr, handler);

  if (error == Error::COMM_ALREADY_CONNECTED) {
    conn_state->connected = true;
    conn_state->cond.notify_all();
  }
  else if (error != Error::OK) {
    if (conn_state->service_name != "") {
      HT_ERRORF("Connection attempt to %s at %s failed - %s.  Will retry "
                "again in %d milliseconds...", conn_state->service_name.c_str(),
                conn_state->addr.format().c_str(), Error::get_text(error),
                (int)conn_state->timeout_ms);
    }
    else {
      HT_ERRORF("Connection attempt to service at %s failed - %s.  Will retry "
                "again in %d milliseconds...", conn_state->addr.format()
                .c_str(), Error::get_text(error), (int)conn_state->timeout_ms);
    }

    // reschedule (throw in a little randomness)
    boost::xtime_get(&conn_state->next_retry, boost::TIME_UTC);
    xtime_add_millis(conn_state->next_retry, conn_state->timeout_ms);

    int32_t milli_adjust = System::rand32() % 2000;
    if (System::rand32() & 1)
      xtime_sub_millis(conn_state->next_retry, milli_adjust);
    else
      xtime_add_millis(conn_state->next_retry, milli_adjust);

    // add to retry heap
    m_impl->retry_queue.push(conn_state);
    m_impl->retry_cond.notify_one();
  }

}


int ConnectionManager::remove(struct sockaddr_in &addr) {
  bool do_close = false;
  int error = Error::OK;

  {
    ScopedLock lock(m_impl->mutex);
    SockAddrMap<ConnectionStatePtr>::iterator iter =
        m_impl->conn_map.find(addr);

    if (iter != m_impl->conn_map.end()) {
      ScopedLock conn_lock((*iter).second->mutex);
      if ((*iter).second->connected)
        do_close = true;
      else
        (*iter).second->connected = true;  // prevent further attempts
      m_impl->conn_map.erase(iter);
    }
  }

  if (do_close)
    error = m_impl->comm->close_socket(addr);

  return error;
}



/**
 * This is the AsyncComm dispatch handler method.  It gets called for each
 * connection related event (establishment, disconnect, etc.) for each
 * connection.  For connect events, the connection's connected flag is set to
 * true and it's condition variable is signaled.  For all other events (e.g.
 * disconnect or error), the connection's connected flag is set to false and a
 * retry is scheduled.
 *
 * @param event_ptr shared pointer to event object
 */
void
ConnectionManager::handle(EventPtr &event_ptr) {
  ScopedLock lock(m_impl->mutex);
  SockAddrMap<ConnectionStatePtr>::iterator iter =
      m_impl->conn_map.find(event_ptr->addr);

  if (iter != m_impl->conn_map.end()) {
    ScopedLock conn_lock((*iter).second->mutex);
    ConnectionState *conn_state = (*iter).second.get();

    if (event_ptr->type == Event::CONNECTION_ESTABLISHED) {
      conn_state->connected = true;
      conn_state->cond.notify_all();
    }
    else if (event_ptr->type == Event::ERROR ||
             event_ptr->type == Event::DISCONNECT) {
      if (!m_impl->quiet_mode) {
        HT_INFOF("%s; Problem connecting to %s, will retry in %d "
                 "milliseconds...", event_ptr->to_str().c_str(),
                 conn_state->service_name.c_str(), (int)conn_state->timeout_ms);
      }
      conn_state->connected = false;
      // this logic could proably be smarter.  For example, if the last
      // connection attempt was a long time ago, then schedule immediately
      // otherwise, if this event is the result of an immediately prior connect
      // attempt, then do the following
      boost::xtime_get(&conn_state->next_retry, boost::TIME_UTC);
      xtime_add_millis(conn_state->next_retry, conn_state->timeout_ms);

      // add to retry heap
      m_impl->retry_queue.push(conn_state);
      m_impl->retry_cond.notify_one();
    }

    // Chain event to application supplied handler
    if (conn_state->handler)
      conn_state->handler->handle(event_ptr);
  }
  else {
    HT_WARNF("Unable to find connection for %s in map.",
             InetAddr::format(event_ptr->addr).c_str());
  }
}


/**
 * This is the boost::thread run method.
 */
void ConnectionManager::operator()() {
  ScopedLock lock(m_impl->mutex);
  ConnectionStatePtr conn_state;

  while (!m_impl->shutdown) {

    while (m_impl->retry_queue.empty()) {
      m_impl->retry_cond.wait(lock);
      if (m_impl->shutdown)
        break;
    }

    if (m_impl->shutdown)
      break;

    conn_state = m_impl->retry_queue.top();

    if (!conn_state->connected) {
      {
        ScopedLock conn_lock(conn_state->mutex);
        boost::xtime now;
        boost::xtime_get(&now, boost::TIME_UTC);

        if (xtime_cmp(conn_state->next_retry, now) <= 0) {
          m_impl->retry_queue.pop();
          send_connect_request(conn_state.get());
          continue;
        }
      }
      m_impl->retry_cond.timed_wait(lock, conn_state->next_retry);
    }
    else
      m_impl->retry_queue.pop();
  }
}


