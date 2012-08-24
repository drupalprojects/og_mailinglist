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

  $query = db_select('og_mailinglist_group_subscription', 'ogms');
  $query->join('node', 'n', 'ogms.nid = n.nid');
  $query->condition('n.created', $digest_start, '>');
  $query->condition('n.status', 1);
  $query->condition('ogms.subscription_type', 'digest email');
  $query->addField('ogms', 'nid');
  $groups_with_new_nodes = $query->execute();

  $query = db_select('og_mailinglist_group_subscription', 'ogms');
  $query->join('comment', 'c', 'ogms.nid = c.nid');
  $query->condition('c.created', $digest_start, '>');
  $query->condition('c.status', '1');
  $query->condition('ogms.subscription_type', 'digest email');
  $query->addField('ogms', 'nid');
  $groups_with_new_comments = $query->execute();

  $digest_groups = array();
  foreach ($groups_with_new_comments as $data) {
    $digest_groups[$data->nid] = $data->nid;
  }
  foreach ($groups_with_new_nodes as $data) {
    $digest_groups[$data->nid] = $data->nid;
  }

  foreach ($digest_groups as $gid) {
    $group = node_load($gid);
    $group_name = _og_mailinglist_get_group_name($group_node);
    $server = variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']);
    $group_email = $group_name . '@' . $server;

    // Get list of new activity -- new nodes and new comments
    $query = db_select('og_membership', 'ogm');
    $query->condition('ogm.entity_type', 'node');
    $query->condition('ogm.created', $digest_start, '>');
    $query->condition('ogm.state', 1);
    $query->condition('ogm.gid', $gid);
    $query->addField('ogm', 'etid');
    $nids_with_new_nodes = $query->execute();

    $query = db_select('comment', 'c');
    $query->join('og_membership', 'ogm', 'c.nid = ogm.etid');
    $query->condition('ogm.entity_type', 'node');
    $query->condition('ogm.state', 1);
    $query->condition('ogm.gid', 1);
    $query->condition('c.created', $digest_start, '>');
    $query->condition('c.status', 1);
    $query->fields('c', array('cid', 'nid'));
    $query->addField('ogm', 'etid');
    $nids_with_new_comments = $query->execute();

    $nids = array();
    foreach ($nids_with_new_comments as $data) {
      $nid = $data->etid;
      $nids[$nid]['status'] = "old";
      $nids[$nid]['node_obj'] = node_load($nid);
      $nids[$nid]['comments'][$data->cid] = _comment_load($data->cid);
    }
    foreach ($nids_with_new_nodes as $data) {
      $nid = $data->etid;
      $nids[$nid]['status'] = "new";
      $nids[$nid]['node_obj'] = node_load($nid);
    }

    // Count number of messages.
    $message_count = 0;
    foreach ($nids as $nid) {
      $message_count += og_mailinglist_count_new_messages($nid);
    }

    $messages = format_plural($message_count, '1 message', '@count messages');
    $discussions = format_plural(count($nids), '1 discussion', '@count discussions');
    $subject = t('[@ogmemail] Digest for @groupemail - @messages in @discussions',
      array(
        '@ogmemail' => $group_name,
        '@groupemail' => $group_email,
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
    foreach ($nids as $nid => $node) {
      $body .= "<li>" . l($node['node_obj']->title, "",
        array(
          'fragment' => "digest-" . $nid,
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
    foreach ($nids as $nid => $node) {
      $body .= '<a name="digest-' . $nid . '"></a>';
      $body .= '<div style="' . DISCUSSION_HEADER . '">';
      $body .= t("Discussion: @link",
        array(
          '@link' => l($node['node_obj']->title,
            "node/" . $nid,
            array('absolute' => TRUE))
        ))
        . "\n";
      $body .= "</div>";
      $body .= "<blockquote>\n";
      // If new node created today.
      if ($node['status'] === "new") {
        $body .= og_mailinglist_style_node_message($node['node_obj']);
      }

      foreach ($node['comments'] as $cid => $comment) {
        $body .= og_mailinglist_style_comment_message($comment);
      }
      $body .= "</blockquote>\n";
    }

    // Add footer.
    $body .= "__<br />";
    $body .= t("You received this message because you are a member of the \"@group\" group on @server",
      array(
        '@group' => $group->title,
        '@server' => $server,
      ))
     . "<br />";
    $body .= t("To unsubscribe from this group, visit @url",
      array('@url' => url("og_mailinglist/subscriptions", array("absolute" => TRUE))))
      . "<br />";
    $body .= t("To post a new message to this group, email @email",
      array(
        '@email' =>  $group_email,
      ))
      . "<br />";

    // For each person, send out an email.
    $query = db_select('og_mailinglist_group_subscription', 'ogms');
    $query->join('users', 'u', 'ogms.uid = u.uid');
    $query->condition('ogms.nid', $gid);
    $query->condition('ogms.subscription_type', 'digest email');
    $query->addField('u', 'mail');
    $emails = $query->execute();

    // Add a test user if any.
    if ($uid = variable_get('og_mailinglist_test_email_address', '')) {
      $email = db_query("SELECT mail FROM {users} WHERE uid = :uid",
        array(':uid' => $uid))->fetchField();
      if (!in_array($email, $emails)) {
        $emails[] = $email;
      }
    }

    foreach ($emails as $email) {
      $email = $email->mail;
      // Ensure email is valid.
      if (!valid_email_address($email)) {
        continue;
      }

      $mailer = og_mailinglist_create_mailer();
      $mailer->From = $group->group_name . "+no-reply@" . $server_name;
      $mailer->FromName = $group_email . "@" . $server_name;
      $mailer->AddCustomHeader($mailer->HeaderLine("List-Id", "<" . $group_email . ">"));
      $mailer->AddCustomHeader($mailer->HeaderLine("List-Post", "<mailto:" . $group_email . ">"));
      $mailer->AddCustomHeader($mailer->HeaderLine("List-Archive", url("node/" .
            $group->nid, array('absolute' => TRUE))));
      $mailer->AddAddress($email);
      $mailer->Subject = $subject;
      $mailer->Body = $body;
      $mailer->isHTML(TRUE);
      $mailer->Send();
      watchdog('og_mailinglist', 'Sent digest email to @email for group @group',
                array('@email' => $group_email, '@group' => $group->title), WATCHDOG_INFO);
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
  $name = isset($user->realname) ? $user->realname : $user->name;
  $body = "<div style=\"" . MESSAGE_HEADER . "\"><strong>" .
    l($name, "user/" . $user->uid, array('absolute' => TRUE))
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
  $count += count($message['comments']);

  return $count;
}
