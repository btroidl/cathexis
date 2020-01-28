/*
 * IRC - Internet Relay Chat, ircd/m_topic.c
 * Copyright (C) 1990 Jarkko Oikarinen and
 *                    University of Oulu, Computing Center
 *
 * See file AUTHORS in IRC package for additional names of
 * the programmers.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 1, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * $Id: m_topic.c 1938 2010-01-07 03:23:57Z entrope $
 */

#include "config.h"

#include "channel.h"
#include "client.h"
#include "hash.h"
#include "ircd.h"
#include "ircd_features.h"
#include "ircd_log.h"
#include "ircd_reply.h"
#include "ircd_snprintf.h"
#include "ircd_string.h"
#include "msg.h"
#include "numeric.h"
#include "numnicks.h"
#include "send.h"

/* #include <assert.h> -- Now using assert in ircd_log.h */
#include <stdlib.h> /* for atoi() */

/** Set a channel topic or report an error.
 * @param[in] sptr Original topic setter.
 * @param[in] cptr Neighbor that sent the topic message.
 * @param[in] chptr Channel to set topic on.
 * @param[in] topic New topic.
 * @param[in] ts Timestamp that topic was set (0 for current time).
 */
static void do_settopic(struct Client *sptr, struct Client *cptr,
		        struct Channel *chptr, char *topic, time_t ts, char *setter)
{
   struct Client *from;
   char* p = 0;
   char* nicktok = NULL;
   char nick[NICKLEN + 1];
   int newtopic;

   if (feature_bool(FEAT_HIS_BANWHO) && IsServer(sptr))
       from = &his;
   else
       from = sptr;
   /* Note if this is just a refresh of an old topic, and don't
    * send it to all the clients to save bandwidth.  We still send
    * it to other servers as they may have split and lost the topic.
    */
   newtopic=ircd_strncmp(chptr->topic,topic,TOPICLEN)!=0;
   /* setting a topic */
   ircd_strncpy(chptr->topic, topic, TOPICLEN);
   if (setter != NULL) {
     memset(chptr->topic_nick, 0, NICKLEN + USERLEN + HOSTLEN + 3);
     ircd_strncpy(chptr->topic_nick, setter, NICKLEN + USERLEN + HOSTLEN + 3);
     nicktok = ircd_strtok(&p, setter, "!");
     if (nicktok != NULL)
       ircd_strncpy((char *)&nick, nicktok, NICKLEN);
   } else {
     memset(chptr->topic_nick, 0, NICKLEN + USERLEN + HOSTLEN + 3);
     if (feature_bool(FEAT_HOST_IN_TOPIC)) {
       ircd_snprintf(0, chptr->topic_nick, NICKLEN + USERLEN + HOSTLEN + 3, "%s!%s@%s",
                     cli_name(from), cli_user(from)->username, cli_user(from)->host);
     } else {
       ircd_strncpy(chptr->topic_nick, cli_name(from), NICKLEN + USERLEN + HOSTLEN + 3);
     }
     ircd_strncpy((char *)&nick, cli_name(from), NICKLEN);
   }
   chptr->topic_time = ts ? ts : TStime();
   /* Fixed in 2.10.11: Don't propagate local topics */
   if (!IsLocalChannel(chptr->chname))
   {
     if (setter != NULL)
       sendcmdto_serv_butone(sptr, CMD_TOPIC, cptr, "%H %s %Tu %Tu :%s", chptr,
                             chptr->topic_nick, chptr->creationtime,
                             chptr->topic_time, chptr->topic);
     else
       sendcmdto_serv_butone(sptr, CMD_TOPIC, cptr, "%H %Tu %Tu :%s", chptr,
                             chptr->creationtime, chptr->topic_time,
                             chptr->topic);
   }
   if (newtopic)
   {
     struct Membership *member;

     /* If the member is delayed-join, show them. */
     member = find_channel_member(sptr, chptr);
     if (member && IsDelayedJoin(member))
       RevealDelayedJoin(member);

     sendcmdto_channel_butserv_butone(from, CMD_TOPIC, chptr, NULL, 0,
      				       (setter ? "%H :%s (%s)" : "%H :%s%s"),
                                       chptr, chptr->topic, (setter ? nick : ""));
   }
      /* if this is the same topic as before we send it to the person that
       * set it (so they knew it went through ok), but don't bother sending
       * it to everyone else on the channel to save bandwidth
       */
    else if (MyUser(sptr))
      sendcmdto_one(sptr, CMD_TOPIC, sptr, "%H :%s", chptr, chptr->topic);
}

/** Handle a local user's attempt to get or set a channel topic.
 *
 * \a parv has the following elements:
 * \li \a parv[1] is the channel name
 * \li \a parv[\a parc - 1] is the topic (if \a parc > 2)
 *
 * See @ref m_functions for discussion of the arguments.
 * @param[in] cptr Client that sent us the message.
 * @param[in] sptr Original source of message.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 */
int m_topic(struct Client* cptr, struct Client* sptr, int parc, char* parv[])
{
  struct Channel *chptr;
  char *topic = 0, *name, *p = 0;
  const char *topicnocolor = 0;
  int hascolor = 0;

  if (parc < 2)
    return need_more_params(sptr, "TOPIC");

  if (parc > 2) {
    topic = parv[parc - 1];

    hascolor = HasColor(topic);
    if (hascolor)
      topicnocolor = StripColor(topic);
  }

  for (; (name = ircd_strtok(&p, parv[1], ",")); parv[1] = 0)
  {
    chptr = 0;
    /* Does the channel exist */
    if (!IsChannelName(name) || !(chptr = FindChannel(name)))
    {
    	send_reply(sptr,ERR_NOSUCHCHANNEL,name);
    	continue;
    }
    /* Trying to check a topic outside a secret channel */
    if ((topic || SecretChannel(chptr)) && !find_channel_member(sptr, chptr))
    {
      send_reply(sptr, ERR_NOTONCHANNEL, chptr->chname);
      continue;
    }

    /* only asking for topic */
    if (!topic)
    {
      if (chptr->topic[0] == '\0')
	send_reply(sptr, RPL_NOTOPIC, chptr->chname);
      else
      {
	send_reply(sptr, RPL_TOPIC, chptr->chname, chptr->topic);
	send_reply(sptr, RPL_TOPICWHOTIME, chptr->chname, chptr->topic_nick,
		   chptr->topic_time);
      }
      continue;
    }
    else if ((chptr->mode.mode & MODE_TOPICLIMIT) &&
             !is_chan_op(sptr, chptr) && !is_half_op(sptr, chptr))
    {
      send_reply(sptr, ERR_CHANOPRIVSNEEDED, chptr->chname);
      continue;
    }
    else if (!client_can_send_to_channel(sptr, chptr, 1))
    {
      send_reply(sptr, ERR_CANNOTSENDTOCHAN, chptr->chname);
      continue;
    }

    if (hascolor && (chptr->mode.exmode & EXMODE_NOCOLOR))
    {
      send_reply(sptr, ERR_CANNOTSENDTOCHAN, chptr->chname);
      continue;
    }

    if (hascolor && (chptr->mode.exmode & EXMODE_STRIPCOLOR))
    {
      do_settopic(sptr,cptr,chptr,(char*)topicnocolor,0,NULL);
      continue;
    }

    do_settopic(sptr,cptr,chptr,topic,0,NULL);
  }
  return 0;
}

/** Handle a remote user's attempt to set a channel topic.
 * \a parv has the following elements:
 * \li \a parv[1] is the channel name
 * \li \a parv[\a parc - 4] is the topic setter (optional)
 * \li \a parv[\a parc - 3] is the channel creation timestamp (optional)
 * \li \a parv[\a parc - 2] is the topic's timestamp (optional)
 * \li \a parv[\a parc - 1] is the topic
 *
 * See @ref m_functions for discussion of the arguments.
 * @param[in] cptr Client that sent us the message.
 * @param[in] sptr Original source of message.
 * @param[in] parc Number of arguments.
 * @param[in] parv Argument vector.
 */
int ms_topic(struct Client* cptr, struct Client* sptr, int parc, char* parv[])
{
  struct Channel *chptr;
  char *topic = 0, *name, *p = 0;
  char *setter = NULL;
  time_t ts = 0;

  if (parc < 3)
    return need_more_params(sptr, "TOPIC");

  topic = parv[parc - 1];

  for (; (name = ircd_strtok(&p, parv[1], ",")); parv[1] = 0)
  {
    chptr = 0;
    /* Does the channel exist */
    if (!IsChannelName(name) || !(chptr = FindChannel(name)))
    {
    	send_reply(sptr,ERR_NOSUCHCHANNEL,name);
    	continue;
    }

    /* Ignore requests for topics from remote servers */
    if (IsLocalChannel(name) && !MyUser(sptr))
    {
      protocol_violation(sptr,"Topic request");
      continue;
    }

    if (parc > 5)
      setter = parv[parc - 4];

    /* If existing channel is older or has newer topic, ignore */
    if (parc > 4 && (ts = atoi(parv[parc - 3])) && chptr->creationtime < ts)
      continue;

    ts = 0; /* Default to the current time if no topic_time is passed. */
    if (parc > 3 && (ts = atoi(parv[parc - 2])) && chptr->topic_time > ts)
      continue;

    do_settopic(sptr,cptr,chptr,topic, ts, setter);
  }
  return 0;
}
