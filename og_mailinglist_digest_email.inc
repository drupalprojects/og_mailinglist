<?php

require_once('og_mailinglist_utilities.inc');

define("EMAIL_HEADER", 'background-color: #D1DCFF; font-family: helvetica;
       font-size: 160%; border-top: 1px solid #262c40; padding: 2px');
define("DISCUSSION_HEADER", 'background-color: #D1DCFF; font-family: helvetica;
       font-size: 140%; border-top: 1px solid #262c40; padding: 2px');
define("MESSAGE_HEADER", 'background-color: #E6ECFF; font-family: helvetica;
       border-top: 1px solid #262c40; padding: 2px');
define("DIGEST_TIME_PERIOD", 86400); // One day currently.
// TODO -- make email from list address

function _og_mailinglist_send_digest_emails() {
  // Digest start time (current time minus the digest time period)
  $digest_start = time() - DIGEST_TIME_PERIOD;

  // Get list of groups where at least one person has subscribed to a digest node
  // and which had a new post or comment in the last 24 hours.

  $new_nodes_sql = "SELECT DISTINCT s.nid
            FROM {og_mailinglist_subscription} s
            JOIN {og_ancestry} o
            ON (s.nid = o.group_nid)
            JOIN {node} n
            ON (o.nid = n.nid)
            WHERE n.created > %d
            AND s.subscription_type = 'digest email'";

  $new_comments_sql = "SELECT DISTINCT s.nid
            FROM {og_mailinglist_subscription} s
            JOIN {og_ancestry} o
            ON (s.nid = o.group_nid)
            JOIN {comments} c
            ON (o.nid = c.nid)
            WHERE c.status = 0
            AND c.timestamp > %d
            AND s.subscription_type = 'digest email'";

  $groups_with_new_nodes = db_query($new_nodes_sql, $digest_start);
  $groups_with_new_comments = db_query($new_comments_sql, $digest_start);

  $digest_groups = array();
  while ($data = db_fetch_array($groups_with_new_comments)) {
    $digest_groups[$data['nid']] = $data['nid'];
  }
  while ($data = db_fetch_array($groups_with_new_nodes)) {
    $digest_groups[$data['nid']] = $data['nid'];
  }

  foreach ($digest_groups as $gid) {
    $group = node_load(array('nid' => $gid));
    // Get list of new activity -- new nodes and new comments
    $new_nids = "SELECT o.nid
                  FROM {node} n
                  JOIN {og_ancestry} o
                  ON n.nid = o.nid
                  WHERE n.created > %d
                  AND o.group_nid = %d";

    $new_comment_nids = "SELECT c.nid, c.cid
                          FROM {comments} c
                          JOIN {og_ancestry} o
                          ON c.nid = o.nid
                          WHERE c.status = 0
                          AND c.timestamp > %d
                          AND o.group_nid = %d";

    $nids_with_new_nodes = db_query($new_nids, $digest_start, $gid);
    $nids_with_new_comments = db_query($new_comment_nids, $digest_start, $gid);

    $nids = array();
    while ($data = db_fetch_array($nids_with_new_comments)) {
      $nids[$data['nid']]['status'] = "old";
      $nids[$data['nid']]['node_obj'] = node_load(array("nid" => $data['nid']));
      $nids[$data['nid']][$data['cid']] = _comment_load($data['cid']);
    }
    while ($data = db_fetch_array($nids_with_new_nodes)) {
      $nids[$data['nid']]['status'] = "new";
      $nids[$data['nid']]['node_obj'] = node_load(array("nid" => $data['nid']));
    }

    // Count # of messages.
    $message_count = 0;
    foreach ($nids as $nid) {
      $message_count += og_mailinglist_count_new_messages($nid);
    }

    $group_email = db_result(db_query("SELECT group_email
                                        FROM {og_mailinglist}
                                        WHERE nid = %d", $gid));
    $subject = t('[@group_name] Digest for @group_title - @messages_plural in @discussions_plural',
      array(
        '@group_name' => $group->ogm_email,
        '@group_title' => $group_email . "@" . variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']),
        '@messages_plural' => format_plural($message_count, '1 message', '@count messages'),
        '@discussions_plural' => format_plural(count($nids), '1 discussion', '@count discussions'),
    ));

    // Assemble message
    $body = "";
    $body .= "<div style=\"" . EMAIL_HEADER . "\">" . t("Today's Discussion Summary") . "</div>\n";
    $body .= t("Group") . ": " . url("node/" . $gid, array('absolute' => TRUE)) . "\n";
    $body .= "<ul>\n";
    foreach ($nids as $nid) {
      $body .= "<li>" . l($nid['node_obj']->title, "",
              array('fragment' => "digest-" . $nid['node_obj']->nid, 'external' => true))
              . " (" . format_plural(og_mailinglist_count_new_messages($nid), "1 new", "@count new") . ")</li>\n";
    }
    $body .= "</ul>\n";
    $body .= "<hr />\n";

    // Add individual posts.
    foreach ($nids as $nid) {
      $body .= '<a name="digest-' . $nid['node_obj']->nid . '"></a>';
      $body .= '<div style="' . DISCUSSION_HEADER . '">';
      $body .= t("Discussion") . ": " . l($nid['node_obj']->title, "node/" .
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
    $body .= "__<br />\n";
    $body .= t("You received this message because you are a member of the \"@group\" group on !server",
      array('@group' => $group->title, '!server' => variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']))) . "<br />\n";
    $body .= t("To unsubscribe from this group, visit !url",
      array('!url' => url("og_mailinglist/subscriptions", array("absolute" => true)))) . "<br />\n";
    $body .= t("To post a new message to this group, email to !email",
      array('!email' => $group_email . "@" . variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']))) . "<br />\n";

    // For each person, send out an email.
    $result = db_query("SELECT uid
                       FROM {og_mailinglist_subscription}
                       WHERE nid = %d
                       AND subscription_type = 'digest email'", $gid);
    $uids = array();
    while ($data = db_fetch_array($result)) {
      $email = db_result(db_query("SELECT mail
                                  FROM {users}
                                  WHERE uid = %d", $data['uid']));
      $uids[$data['uid']] = $email;
    }

    // Add a test user if any.
    if ($uid = variable_get('og_mailinglist_test_email_address', '')) {
      $email = db_result(db_query("SELECT mail FROM {users} WHERE uid = %d", $uid));
      $uids[$uid] = $email;
    }

    foreach ($uids as $email) {
      // Ensure email is valid.
      if (!valid_email_address($email)) {
        continue;
      }
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

function og_mailinglist_style_node_message($node) {
  $user = user_load(array('uid' => $node->uid));
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

function og_mailinglist_style_comment_message($comment) {
  $user = user_load(array('uid' => $comment->uid));
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

function og_mailinglist_count_new_messages($message) {
    $count = 0;
    if ($message['status'] === "new") {
      $count++;
    }
    $count += count(array_keys($message)) - 2;

    return $count;
}
