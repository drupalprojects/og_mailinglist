<?php
/**
 * @file
 * API functions for OG Mailinglist.
 */

/**
 * Saves new thread subscriptions for members of a group.
 *
 * @param int $gid Group ID
 * @param int $nid Node ID
 */
function og_mailinglist_save_group_thread_subscriptions($gid, $nid) {
  $subscribers = og_mailinglist_get_group_subscriptions($gid);
  // Clean bad email addresses from the array.
  foreach ($subscribers as $uid => $email) {
    if (!valid_email_address($email)) {
      unset($subscribers[$uid]);
    }
  }
  // Save users to og_mailinglist_thread_subscription
  og_mailinglist_save_thread_subscriptions($nid, array_keys($subscribers));
}
/**
 * Returns users who are active email subscribers in a group.
 *
 * @param int $gid Group id to query for subscriptions
 *
 * @return array of UIDs and email addresses that are subscribed to this group.
 *  Format = key (UID) => value (email address)
 */
function og_mailinglist_get_group_subscriptions($gid) {
  $query = db_select('og_mailinglist_group_subscription', 's');
  $query->join('users', 'u', 's.uid = u.uid');
  $query->condition('s.nid', $gid);
  $query->condition('s.subscription_type', 'email');
  $query->fields('u', array('uid', 'mail'));
  $emails = $query->execute()->fetchAllKeyed();

  return $emails;
}

/*
 * @return array of UIDs and email addresses that are subscribed to this thread.
 *  Format = key (UID) => value (email address)
 */
function og_mailinglist_get_thread_subscriptions($nid) {
  $query = db_select('og_mailinglist_thread_subscription', 'ogmt');
  $query->join('users', 'u', 'ogmt.uid = u.uid');
  $query->condition('ogmt.nid', $nid);
  $query->condition('u.status', 1);
  $query->fields('u', array('uid', 'mail'));
  $result = $query->execute();

  $emails = array();
  foreach ($result as $user) {
    $emails[$user->uid] = $user->mail;
  }

  return $emails;
}

/*
 * Check if a user is subscribed to a thread.
 */
function og_mailinglist_check_user_subscribed_to_thread($nid, $uid) {
  $query = db_select('og_mailinglist_thread_subscription', 'ogmt');
  $query->condition('ogmt.nid', $nid);
  $query->condition('ogmt.uid', $uid);
  $query->countQuery();
  return $query->execute()->fetchField();
}

/*
 * @param int $nid Node id for thread.
 * @param array $uids array of User IDs that are subscribed to this thread.
 *
 * @return null.
 */
function og_mailinglist_save_thread_subscriptions($nid, $uids) {
  static $saved = array();
  foreach ($uids as $uid) {
    // Check that this subscription hasn't already been saved on this page load.
    if (in_array($nid . "_" . $uid, $saved)) {
      continue;
    }
    $saved[] = $nid . "_" . $uid;

    if ($uid != 0) { // Ignore anonymous users.
      $data = array(
        "nid" => $nid,
        "uid" => $uid,
      );

      $result = db_merge('og_mailinglist_thread_subscription')
        ->key(array('nid' => $nid, 'uid' => $uid))
        ->execute();

      if ($result) {
        // Invoke hook
        module_invoke_all('og_mailinglist_save_thread_subscription', $nid, $uid);
      }
    }
  }
}

/*
 * @return subscription type for the given group and user.
 */
function og_mailinglist_get_group_subscription_type($gid, $uid) {
  $query = db_select('og_mailinglist_group_subscription', 'ogms');
  $query->fields('ogms', array('subscription_type'));
  $query->condition('ogms.nid', $gid);
  $query->condition('ogms.uid', $uid);
  return $query->execute()->fetchField();
}

/*
 * Update a user's subscription type for one of their groups.
 */
function og_mailinglist_update_group_subcription_type($gid, $uid, $subscription_type) {
  db_update('og_mailinglist_group_subscription')
    ->fields(array('subscription_type' => $subscription_type))
    ->condition('uid', $uid)
    ->condition('nid', $gid)
    ->execute();
}

/*
 * @param int $gid Group id.
 * @param array $uids array of User IDs to add group subscriptions.
 */
function og_mailinglist_save_group_subscriptions($gid, $uids) {
  $subscription_type = variable_get('og_mailinglist_default_group_subscription_type', 'email');
  foreach ($uids as $uid) {
    db_merge('og_mailinglist_group_subscription')
      ->key(array('nid' => $gid, 'uid' => $uid))
      ->fields(array(
          'subscription_type' => $subscription_type,
      ))
      ->execute();
  }
}

/*
 * Delete group subscription for all user IDs in $uids.
 *
 * @param int $gid Group id.
 * @param array $uids array of User IDs to add group subscriptions.
 */
function og_mailinglist_delete_group_subscriptions($gid, $uids) {
  db_delete('og_mailinglist_group_subscription')
    ->condition('nid', $gid)
    ->condition('uid', $uids, 'IN')
    ->execute();
  foreach ($uids as $uid) {
    og_mailinglist_delete_group_threads($gid, $uid);
  }
}

/**
 * Unsubscribe a user from all group threads when they leave that group.
 **/
function og_mailinglist_delete_group_threads($gid, $uid) {
  // Delete threads.  See http://drupal.org/node/1079620
  $subquery = db_select('og_membership', 'o');
  $subquery->addField('o', 'etid');
  $subquery->condition('entity_type', 'node');
  $subquery->condition('gid', $gid);

  return db_delete('og_mailinglist_thread_subscription')
    ->condition('uid', $uid)
    ->condition('nid', $subquery, 'IN')
    ->execute();
}

/**
 * Remove a user from a thread.
 **/
function og_mailinglist_delete_thread_subscription($nid, $uid) {
  static $deleted = array();
  // Check that this subscription hasn't already been deleted.
  if (in_array($nid . "_" . $uid, $deleted)) {
    return;
  }
  $deleted[] = $nid . "_" . $uid;

  $result = db_delete('og_mailinglist_thread_subscription')
    ->condition('nid', $nid)
    ->condition('uid', $uid)
    ->execute();

  if ($result) {
    // Invoke hook
    module_invoke_all('og_mailinglist_delete_thread_subscription', $nid, $uid);
  }

  return $result;
}

/*
 * Log successful sent email.
 */
function og_mailinglist_log_email_sent($source, $nid, $cid = 0, $message_id = 0, $in_reply_to = 0, $references = 0, $parent_message_id = 0) {
  $data = array(
    'nid' => $nid,
    'cid' => $cid,
    'source' => $source,
    'message_id' => $message_id,
    'in_reply_to' => $in_reply_to,
    'references_header' => $references,
    'parent_message_id' => $parent_message_id,
  );
  return drupal_write_record('og_mailinglist_source', $data);
}

/*
 * @return PHPMailer object.
 */
function og_mailinglist_create_mailer() {
  if (og_mailinglist_phpmailer_load_library()) {
    // The TRUE param makes it throw exceptions on errors, which we need to catch.
    $mailer = new PHPMailer(TRUE);
    $mailer->PluginDir = og_mailinglist_get_phpmailer_plugin_dir();
    $mailer->CharSet = 'UTF-8';
    drupal_alter('og_mailinglist_new_phpmailer', $mailer);

    if ($mailer) {
      return $mailer;
    }
  }

  drupal_set_message(t('E-mail cannot be sent. PHPMailer library is missing.'
    . ' See <a href=@link>status report</a> for more information.',
    array('@link' => '/admin/reports/status')), 'error');
}

/*
 * Delete group information.
 */
function og_mailinglist_delete_group($nid) {
  // Delete group subscriptions from og_mailinglist_group_subscription.
  db_delete('og_mailinglist_group_subscription')
    ->condition('nid', $nid)
    ->execute();
}

// References are apparently a more reliable header than in-reply-to
// http://cr.yp.to/immhf/thread.html
function _og_mailinglist_get_thread_parent_messageid(&$headers) {
  // Try to get the parent message-id from one of the message-ids in the references header.
  $references = isset($headers['references']) ? $headers['references'] : '';
  $refs = str_replace(array("\r\n", "\n"), "", $references);
  $refs_array = explode(" ", $refs);

  foreach ($refs_array as $reference) {
    if (!empty($reference)) {
      $parent_message_id = db_query("SELECT parent_message_id
        FROM {og_mailinglist_source}
        WHERE message_id = :message_id",
        array(':message_id' => $reference))
        ->fetchField();

      if (!empty($parent_message_id)) {
        return $parent_message_id;
      }
    }
  }

  // If references don't find anything, try the in-reply-to header.
  if (empty($headers['in-reply-to'])) {
    return;
  }
  $in_reply_to_message_id = $headers['in-reply-to'];
  $parent_message_id = db_query("SELECT parent_message_id
    FROM {og_mailinglist_source}
    WHERE message_id = :message_id",
    array(':message_id' => $in_reply_to_message_id))
    ->fetchField();

  if (!empty($parent_message_id)) {
    return $parent_message_id;
  }
}

function _og_mailinglist_get_nid_from_parent_messageid($message_id) {
  return db_query("SELECT nid
    FROM {og_mailinglist_source}
    WHERE message_id = :message_id",
    array(':message_id' => $message_id))
    ->fetchField();
}

function _og_mailinglist_get_messageid_from_nid($nid) {
  return db_query("SELECT parent_message_id
    FROM {og_mailinglist_source}
    WHERE cid = :cid
    AND nid = :nid",
    array(':cid' => 0, ':nid' => $nid))
    ->fetchField();
}
