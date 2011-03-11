<?php

require_once DRUPAL_ROOT . '/' . 'og_mailinglist_utilities.inc';

define("EMAIL_HEADER", 'background-color: #D1DCFF; font-family: helvetica;
       font-size: 160%; border-top: 1px solid #262c40; padding: 2px');
define("DISCUSSION_HEADER", 'background-color: #D1DCFF; font-family: helvetica;
       font-size: 140%; border-top: 1px solid #262c40; padding: 2px');
define("MESSAGE_HEADER", 'background-color: #E6ECFF; font-family: helvetica;
       border-top: 1px solid #262c40; padding: 2px');
define("DIGEST_TIME_PERIOD", 86400); // One day currently.
// TODO -- make email from list address

function _og_mailinglist_send_digest_emails() {
  // Get list of groups where at least one person has subscribed to a digest node
  // and which had a new post or comment in the last 24 hours.

  $new_nodes_sql = 'SELECT DISTINCT s.nid
            FROM {og_mailinglist_subscription} s
            JOIN {og_ancestry} o
            JOIN {node} n
            WHERE s.nid = o.group_nid
            AND o.nid = n.nid
            AND n.created > (unix_timestamp() - %d)
            AND s.subscription_type = "digest email"';

  $new_comments_sql = 'SELECT DISTINCT s.nid
            FROM {og_mailinglist_subscription} s
            JOIN {og_ancestry} o
            JOIN {comments} c
            WHERE s.nid = o.group_nid
            AND o.nid = c.nid
            AND c.timestamp > (unix_timestamp() - %d)
            AND s.subscription_type = "digest email"';

  $groups_with_new_nodes = db_query('SELECT DISTINCT s.nid
            FROM {og_mailinglist_subscription} s
            JOIN {og_ancestry} o
            JOIN {node} n
            WHERE s.nid = :s.nid
            AND o.nid = :o.nid
            AND n.created > :n.created - %d)
            AND s.subscription_type = :s.subscription_type', array(':s.nid' => : s . nid, ':o.nid' => : o . nid, ':n.created' => : n . created, ':s.subscription_type' => : s . subscription_type, '' => array(
      ':s.nid' => : s . nid,
      ':o.nid' => : o . nid,
      ':n.created' => : n . created,
      ':s.subscription_type' => : s . subscription_type,
      '' => array(':s.nid' => o . group_nid ':o.nid' => n . nid ':n.created' =>),
    )));
  $groups_with_new_comments = db_query('SELECT DISTINCT s.nid
            FROM {og_mailinglist_subscription} s
            JOIN {og_ancestry} o
            JOIN {comments} c
            WHERE s.nid = :s.nid
            AND o.nid = :o.nid
            AND c.timestamp > :c.timestamp - %d)
            AND s.subscription_type = :s.subscription_type', array(':s.nid' => : s . nid, ':o.nid' => : o . nid, ':c.timestamp' => : c . timestamp, ':s.subscription_type' => : s . subscription_type, '' => array(
      ':s.nid' => : s . nid,
      ':o.nid' => : o . nid,
      ':c.timestamp' => : c . timestamp,
      ':s.subscription_type' => : s . subscription_type,
      '' => array(':s.nid' => o . group_nid ':o.nid' => c . nid ':c.timestamp' =>),
    )));

  $digest_groups = array();
  while ($data = db_fetch_array($groups_with_new_comments)) {
    $digest_groups[$data['nid']] = $data['nid'];
  }
  while ($data = db_fetch_array($groups_with_new_nodes)) {
    $digest_groups[$data['nid']] = $data['nid'];
  }

  //print_r($digest_groups);
  foreach ($digest_groups as $gid) {
    $group = node_load($gid);
    // Get list of new activity -- new nodes and new comments
    $new_nids = 'SELECT o.nid
                  FROM {node} n
                  JOIN {og_ancestry} o
                  WHERE n.nid = o.nid
                  AND n.created > (unix_timestamp() - %d)
                  AND o.group_nid = %d';

    $new_comment_nids = 'SELECT c.nid, c.cid
                          FROM {comments} c
                          JOIN {og_ancestry} o
                          WHERE c.nid = o.nid
                          AND c.timestamp > (unix_timestamp() - %d)
                          AND o.group_nid = %d';

    $nids_with_new_nodes = db_query('SELECT o.nid
                  FROM {node} n
                  JOIN {og_ancestry} o
                  WHERE n.nid = :n.nid
                  AND n.created > :n.created - %d)
                  AND o.group_nid = :o.group_nid', array(':n.nid' => : n . nid, ':n.created' => : n . created, ':o.group_nid' => : o . group_nid, '' => array(
        ':n.nid' => : n . nid,
        ':n.created' => : n . created,
        ':o.group_nid' => : o . group_nid,
        '' => array(':n.nid' => o . nid ':n.created' =>),
      )));
    $nids_with_new_comments = db_query('SELECT c.nid, c.cid
                          FROM {comments} c
                          JOIN {og_ancestry} o
                          WHERE c.nid = :c.nid
                          AND c.timestamp > :c.timestamp - %d)
                          AND o.group_nid = :o.group_nid', array(':c.nid' => : c . nid, ':c.timestamp' => : c . timestamp, ':o.group_nid' => : o . group_nid, '' => array(
        ':c.nid' => : c . nid,
        ':c.timestamp' => : c . timestamp,
        ':o.group_nid' => : o . group_nid,
        '' => array(':c.nid' => o . nid ':c.timestamp' =>),
      )));

    $nids = array();
    while ($data = db_fetch_array($nids_with_new_comments)) {
      $nids[$data['nid']]['status'] = "old";
      $nids[$data['nid']]['node_obj'] = node_load($data['nid']);
      $nids[$data['nid']][$data['cid']] = comment_load($data['cid']);
    }
    while ($data = db_fetch_array($nids_with_new_nodes)) {
      $nids[$data['nid']]['status'] = "new";
      $nids[$data['nid']]['node_obj'] = node_load($data['nid']);
    }



    // Count # of messages.
    $message_count = 0;
    foreach ($nids as $nid) {
      $message_count += og_mailinglist_count_new_messages($nid);
    }

    $group_email = db_query("SELECT group_email
                                        FROM {og_mailinglist}
                                        WHERE nid = :nid", array(':nid' => : nid, '' => array(
        ':nid' => : nid,
        '' => array(':nid' => $gid),
      )))->fetchField();

    $subject = "[" . $group->ogm_email . "] Digest for " . $group_email . "@" . variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME'])
          . " - " . $message_count . " Messages in " . count($nids) . " Discussions";

    // Assemble message
    $body = "";
    $body .= "<div style=\"" . EMAIL_HEADER . "\">Today's Discussion Summary</div>\n";
    $body .= "Group: " . url("node/" . $gid, array('absolute' => TRUE)) . "\n";
    $body .= "<ul>\n";
    foreach ($nids as $nid) {
      $body .= "<li>" . l($nid['node_obj']->title, "",
              array(
        'fragment' => "digest-" . $nid['node_obj']->nid,
        'external' => true,
      ))
              . " (" . og_mailinglist_count_new_messages($nid) . " New)</li>\n";
    }
    $body .= "</ul>\n";
    $body .= "<hr />\n";

    // Add individual posts.
    foreach ($nids as $nid) {
      $body .= '<a name="digest-' . $nid['node_obj']->nid . '"></a>';
      $body .= '<div style="' . DISCUSSION_HEADER . '">';
      $body .= "Discussion: " . l($nid['node_obj']->title, "node/" .
          $nid['node_obj']->nid, array('absolute' => TRUE)) . "\n";
      $body .= "</div>";
      $body .= "<blockquote>\n";
      // If new node created today.
      if ($nid['status'] === "new") {
        $body .= og_mailinglist_style_node_message($nid['node_obj']);
      }

      foreach ($nid as $cid => $comment) {
        if (is_numeric($cid)) {
          $body .= og_mailinglist_style_comment_message($comment);
        }
      }
      $body .= "</blockquote>\n";
    }

    // Add footer.
    $body .= "__<br />";
    $body .= "You received this message because you are a member of the \"" .
              $group->title . "\" group on " .
              variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']) . "<br />";
    $body .= "To unsubscribe to this group, visit " .
              url("og_mailinglist/subscriptions", array("absolute" => true)) . "<br />";
    $body .= "To post a new message to this group, email " . $group_email . "@" .
      variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']) . "<br />";

    // For each person, send out an email.
    $result = db_query("SELECT uid
                       FROM {og_mailinglist_subscription}
                       WHERE nid = :nid
                       AND subscription_type = :subscription_type email\"", array(':nid' => : nid, ':subscription_type' => : subscription_type, '' => array(
        ':nid' => : nid,
        ':subscription_type' => : subscription_type,
        '' => array(':nid' => $gid ':subscription_type' =>),
      )));
    $uids = array();
    while ($data = db_fetch_array($result)) {
      $email = db_query("SELECT mail
                                  FROM {users}
                                  WHERE uid = :uid", array(':uid' => : uid, '' => array(
          ':uid' => : uid,
          '' => array(':uid' => $data['uid']),
        )))->fetchField();
      $uids[$data['uid']] = $email;
    }

    // Add a test user if any.
    if ($uid = variable_get('og_mailinglist_test_email_address', '')) {
      $email = db_query("SELECT mail FROM {users} WHERE uid = :uid", array(':uid' => : uid, '' => array(
          ':uid' => : uid,
          '' => array(':uid' => $uid),
        )))->fetchField();
      $uids[$uid] = $email;
    }

    foreach ($uids as $email) {
      $mailer = og_mailinglist_create_mailer();
      $mailer->From = $group->ogm_email . "+no-reply@" . variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']);
      $mailer->FromName = $group_email . "@" .
      variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']);
      $mailer->AddCustomHeader($mailer->HeaderLine("List-Id", "<" . $group->ogm_email .
            "@" . variable_get('og_mailinglist_server_string', $_SERVER['SERVER_NAME']) . ">"));
      $mailer->AddCustomHeader($mailer->HeaderLine("List-Post", "<mailto:" . $group->ogm_email .
            "@" . variable_get('og_mailinglist_server_string', $_SERVER['SERVER_NAME']) . ">"));
      $mailer->AddCustomHeader($mailer->HeaderLine("List-Archive", url("node/" .
            $group->nid, array('absolute' => TRUE))));
      $mailer->AddAddress($email);
      $mailer->Subject = $subject;
      $mailer->Body = $body;
      $mailer->isHTML(TRUE);
      $mailer->Send();
      watchdog('og_mailinglist', 'Sent digest email to @email for group @group',
                array('@email' => $email, '@group' => $group->title), WATCHDOG_INFO);
    }
  }
}

/**
 * @todo Please document this function.
 * @see http://drupal.org/node/1354
 */
function og_mailinglist_style_node_message($node) {
  $user = user_load($node->uid);
  $body .= "<div style=\"" . MESSAGE_HEADER . "\"><strong>" .
    l((isset($user->realname) ? $user->realname : $user->name), "user/" . $user->uid, array('absolute' => TRUE))
    . "</strong> " . $user->mail . " " .
    date("d M Y — g:ia O", $node->created) . "</div>\n";

  // Prepare node body for viewing.
  $node = og_mailinglist_render_node_body($node);
  $body .= og_mailinglist_prepare_web_content($node->body);
  $body .= "<br />\n";

  return $body;
}

/**
 * @todo Please document this function.
 * @see http://drupal.org/node/1354
 */
function og_mailinglist_style_comment_message($comment) {
  // TODO Convert "user_load" to "user_load_multiple" if "$comment->uid" is other than a uid.
  // To return a single user object, wrap "user_load_multiple" with "array_shift" or equivalent.
  // Example: array_shift(user_load_multiple(array(), $comment->uid))
  $user = user_load($comment->uid);
  $body = "<div style=\"" . MESSAGE_HEADER . "\"><strong>" .
    l((isset($user->realname) ? $user->realname : $user->name), "user/" . $user->uid, array('absolute' => TRUE))
    . "</strong> " . $user->mail . " " .
    date("d M Y — g:ia O", $comment->timestamp) . "</div>\n";

  // Filter out quoted sections of emails.
  $comment->comment = _og_mailinglist_remove_quotes($comment->comment);
  $body .= og_mailinglist_prepare_web_content($comment->comment);
  $body .= "<br />\n";

  return $body;
}

/**
 * @todo Please document this function.
 * @see http://drupal.org/node/1354
 */
function og_mailinglist_count_new_messages($message) {
  $count = 0;
  if ($message['status'] === "new") {
    $count++;
  }
  $count += count(array_keys($message)) - 2;

  return $count;
}
