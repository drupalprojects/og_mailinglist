<?php
/**
 * @file
 * Handles sending digest emails. Digest emails are handled as cron jobs.
 *
 */

require_once 'og_mailinglist_utilities.inc';

define("EMAIL_HEADER", 'background-color: #D1DCFF; font-family: helvetica;
       font-size: 160%; border-top: 1px solid #262c40; padding: 2px');
define("DISCUSSION_HEADER", 'background-color: #D1DCFF; font-family: helvetica;
       font-size: 140%; border-top: 1px solid #262c40; padding: 2px');
define("MESSAGE_HEADER", 'background-color: #E6ECFF; font-family: helvetica;
       border-top: 1px solid #262c40; padding: 2px');
// TODO -- make email from list address

function _og_mailinglist_send_digest_emails() {
  $digest_days = variable_get('og_mailinglist_digest_interval_days', 1);
  $digest_start = time() - $digest_days * 86400;

  // Get list of groups where at least one person has subscribed to a digest node
  // and which had a new post or comment in the last 24 hours.

  $groups_with_new_nodes = db_query('SELECT DISTINCT s.nid
            FROM {og_mailinglist_group_subscription} s
            JOIN {og} o
            JOIN {node} n
            ON s.nid = o.group_nid
            WHERE o.etid = n.nid
            AND o.entity_type = "node"
            AND n.created > :digest_time
            AND s.subscription_type = "digest email"',
            array(
              ':digest_time' => $digest_start,
              ':subscription_type' => 'digest email',
            ));

  $groups_with_new_comments = db_query('SELECT DISTINCT s.nid
            FROM {og_mailinglist_group_subscription} s
            JOIN {og} o
            JOIN {comments} c
            ON s.nid = o.etid
            WHERE o.entity_type = "node"
            AND c.status = 0
            AND o.nid = c.nid
            AND c.timestamp > :digest_time
            AND s.subscription_type = :subscription_type',
            array(
              ':digest_time' => $digest_start,
              ':subscription_type' => 'digest email',
            ));

  $digest_groups = array();
  foreach ($groups_with_new_comments as $data) {
    $digest_groups[$data->nid] = $data->nid;
  }
  foreach ($groups_with_new_nodes as $data) {
    $digest_groups[$data->nid] = $data->nid;
  }

  foreach ($digest_groups as $gid) {
    $group = node_load(array('nid' => $gid));
    // Get list of new activity -- new nodes and new comments
    $nids_with_new_nodes = db_query('SELECT o.nid
      FROM {node} n
      JOIN {og} o
      ON n.nid = o.etid
      WHERE o.entity_type = "node"
      AND n.created > :digest_time
      AND o.group_nid = :gid',
      array(
        ':digest_time' => $digest_start,
        ':gid' => $gid,
      ));

    $nids_with_new_comments = db_query('SELECT c.nid, c.cid
      FROM {comments} c
      JOIN {og} o
      ON c.nid = o.etid
      WHERE o.entity_type = "node"
      AND c.status = 0
      AND c.timestamp > :digest_time
      AND o.group_nid = :gid',
      array(
        ':digest_time' => $digest_start,
        ':gid' => $gid,
      ));

    $nids = array();
    foreach ($nids_with_new_comments as $data) {
      $nid = $data->nid;
      $nids[$nid]['status'] = "old";
      $nids[$nid]['node_obj'] = node_load($nid);
      $nids[$nid][$data->cid] = _comment_load($data->cid);
    }
    foreach ($nids_with_new_nodes as $data) {
      $nid = $data->nid;
      $nids[$nid]['status'] = "new";
      $nids[$nid]['node_obj'] = node_load($nid);
    }

    // Count number of messages.
    $message_count = 0;
    foreach ($nids as $nid) {
      $message_count += og_mailinglist_count_new_messages($nid);
    }

    $group_email = db_query("SELECT group_email
                                        FROM {og_mailinglist}
                                        WHERE nid = :nid", array(':nid' => $gid))->fetchField();

    $server_name = variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']);
    $messages = format_plural($message_count, '1 message', '@count messages');
    $discussions = format_plural(count($nids), '1 discussion', '@count discussions');
    $subject = t('[@ogmemail] Digest for @groupemail - @messages in @discussions',
      array(
        '@ogmemail' => $group->group_email,
        '@groupemail' => $group_email . "@" .$server_name,
        '@messages' => $messages,
        '@discussions' => $discussions,
      ));

    // Assemble message
    $body = "";
    $body .= '<div style=\"' . EMAIL_HEADER . '">'
      . t("Today's Discussion Summary")
      . "</div>\n";
    $body .= t("Group: @url",
      array('@url' => url("node/" . $gid, array('absolute' => TRUE)))) . "\n";
    $body .= "<ul>\n";
    foreach ($nids as $nid) {
      $body .= "<li>" . l($nid['node_obj']->title, "",
        array(
          'fragment' => "digest-" . $nid['node_obj']->nid,
          'external' => TRUE
        ))
        . " ("
        . format_plural(og_mailinglist_count_new_messages($nid),
          "1 new", "@count new")
        . ")</li>\n";
    }
    $body .= "</ul>\n";
    $body .= "<hr />\n";

    // Add individual posts.
    foreach ($nids as $nid) {
      $body .= '<a name="digest-' . $nid['node_obj']->nid . '"></a>';
      $body .= '<div style="' . DISCUSSION_HEADER . '">';
      $body .= t("Discussion: @link",
        array(
          '@link' => l($nid['node_obj']->title,
            "node/" . $nid['node_obj']->nid,
            array('absolute' => TRUE))
        ))
        . "\n";
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
    $body .= t("You received this message because you are a member of the \"@group\" group on @server",
      array(
        '@group' => $group->title,
        '@server' => $server_name,
      ))
     . "<br />";
    $body .= t("To unsubscribe from this group, visit @url",
      array('@url' => url("og_mailinglist/subscriptions", array("absolute" => TRUE))))
      . "<br />";
    $body .= t("To post a new message to this group, email @email",
      array(
        '@email' =>  $group_email . "@" . $server_name,
      ))
      . "<br />";

    // For each person, send out an email.
    $emails = db_select('og_mailinglist_group_subscription', 'og')
      ->join('users', 'u', 'og.uid = u.uid')
      ->groupBy('u.uid') // this line might be unnecessary
      ->fields('u.mail')
      ->condition('og.nid', $gid)
      ->condition('og.subscription_type', 'digest email');

    // Add a test user if any.
    if ($uid = variable_get('og_mailinglist_test_email_address', '')) {
      $email = db_query("SELECT mail FROM {users} WHERE uid = :uid",
        array(':uid' => $uid))->fetchField();
      if (!in_array($email, $emails)) {
        $emails[] = $email;
      }
    }

    foreach ($emails as $email) {
      // Ensure email is valid.
      if (!valid_email_address($email)) {
        continue;
      }
      $mailer = og_mailinglist_create_mailer();
      $mailer->From = $group->group_email . "+no-reply@" . $server_name;
      $mailer->FromName = $group_email . "@" . $server_name;
      $mailer->AddCustomHeader($mailer->HeaderLine("List-Id", "<" . $group->group_email .
            "@" . $server_name . ">"));
      $mailer->AddCustomHeader($mailer->HeaderLine("List-Post", "<mailto:" . $group->group_email .
            "@" . $server_name . ">"));
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
 * Wrap node body for including in the digest.
 */
function og_mailinglist_style_node_message($node) {
  $user = user_load(array('uid' => $node->uid));
  $body .= "<div style=\"" . MESSAGE_HEADER . "\"><strong>" .
    l((isset($user->realname) ? $user->realname : $user->name), "user/" . $user->uid, array('absolute' => TRUE))
    . "</strong> " . $user->mail . " " .
    date("d M Y — g:ia O", $node->created) . "</div>\n";

  // Prepare node body for viewing.
  $rendered_body = node_view($node);
  $body = $rendered_body['body'][0]['#markup'];
  $body .= "<br />\n";

  return $body;
}

/**
 * Wrap comment body for including in the digest.
 */
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

/**
 * Count total new messages for a node since the previous digest.
 */
function og_mailinglist_count_new_messages($message) {
  $count = 0;
  if ($message['status'] === "new") {
    $count++;
  }
  $count += count(array_keys($message)) - 2;

  return $count;
}
