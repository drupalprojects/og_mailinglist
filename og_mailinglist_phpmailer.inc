<?php
/**
 * @file
 *  Sending mail using PHPMailer class.
 */

require_once 'og_mailinglist_utilities.inc';
require_once 'og_mailinglist_api.inc';

/**
 * Sends emails for comments.
 *
 * @param $comment
 *  Comment object, for which to send the email.
 *
 * @param $node
 *  Node of the comment.
 *
 * @param $gids
 *  Group IDs of groups where $node belongs.
 */
function og_mailinglist_send_comment_email($comment, $node, $gids) {
  // Check if comment arrived via email so already sent.
  if (_og_mailinglist_email_already_sent($comment->nid, $comment->cid)) {
    return;
  }

  // TODO: Maybe we should sort $gids here, this would ensure that the
  // main group for certain group collections is always the same,
  // older groups would be preferred. But this may already be the case.
  $group_nodes = node_load_multiple($gids);
  
  // Add custom headers.
  $parent_message_id = _og_mailinglist_get_messageid_from_nid($node->nid);
  // If the comment is in reply to another comment, set the in-reply-to header
  // to that comment's message-id, else to the parent post's message-id.
  if ($comment->pid != 0) {
    $data = db_select('og_mailinglist_source', 'ogms')
      ->fields('ogms', array('message_id', 'references_header'))
      ->condition('ogms.cid', $comment->pid)
      ->execute()->fetchAll();

    $in_reply_to = $data->message_id;
    $references = $data->references_header . " " . $in_reply_to;
  }
  else {
    $in_reply_to = $parent_message_id;
    $references = $parent_message_id; // TODO, this should really also reference each message-id that came before it in order. Easy enough to do w/ a query.
  }

  // Decorate subject.
  $subject = 'Re: ' . og_mailinglist_build_subject($group_nodes) . ' ' . $node->title;

  // Decorate body.
  $body = og_mailinglist_prepare_web_content($comment->comment);

  // Add the user's signature.
  $body .= og_mailinglist_build_user_signature($comment->uid);

  // Add the footer
  $body .= og_mailinglist_build_web_footer($node);
  $body_plain_text = drupal_html_to_text($body);

  $messaged_users = array();

  foreach ($group_nodes as $group_node) {
    $mailer = og_mailinglist_create_mailer();
    $mailer->Subject = $subject;
    $mailer->Body = $body;
    $mailer->isHTML = TRUE;
    $mailer->AltBody = $body_plain_text;
    og_mailinglist_add_email_headers($mailer, $node, $group_node, $comment, $in_reply_to, $references);
    _og_mailinglist_add_sender($mailer, $node, $group_node, $comment);
    _og_mailinglist_add_recipients($mailer, $node, $group_node, $messaged_users, FALSE);

    $success = $mailer->Send();
  }
  if ($success) {
    // In case of multiple groups and emails log it only once.
    og_mailinglist_log_email_sent('web', $node->nid, $comment->cid, $mailer->MessageID, $in_reply_to, $references, $parent_message_id);
  }
  else {
    watchdog('og_mailinglist', "OG_Mailinglist couldn't send a new comment email.", NULL, WATCHDOG_ERROR);
  }
}

/**
 * Master function for sending an email for a new node.
 */
function og_mailinglist_send_node_email($node, $gids) {
  // Check if node arrived via email so already sent.
  if (_og_mailinglist_email_already_sent($node->nid)) {
    return;
  }

  $group_nodes = node_load_multiple($gids);

  // Decorate subject.
  $subject = og_mailinglist_build_subject($group_nodes) . " " . $node->title;

  // Prepare node body for viewing.
  $mail_node = og_mailinglist_render_node_body($node);

  // Decorate body.
  $body = og_mailinglist_prepare_web_content($mail_node->body);

  // Add the user's signature.
  $body .= og_mailinglist_build_user_signature($node->uid);

  // Add the footer.
  $body .= og_mailinglist_build_web_footer($node, 0, TRUE);

  $group_emails = _og_mailinglist_get_group_emails($group_nodes);

  // Let other modules alter outgoing node emails.
  $data = array(
    'subject' => $subject,
    'body' => $body,
    'group_mail' => $group_emails[0],
    'group_mails' => $group_emails,
    'title' => $node->title,
    'nid' => $node->nid,
    'node' => $node,
  );

  drupal_alter('og_mailinglist_new_node', $data);

  // Keep track of users who already got this email since they are members of
  // other groups.
  $messaged_users = array();

  foreach ($group_nodes as $group_node) {
    $group_email = $group_node->group_email[LANGUAGE_NONE][0]['value'];

    $mailer = og_mailinglist_create_mailer();
    $mailer->Subject = $data['subject'];
    $mailer->isHTML = TRUE;
    $mailer->Body = $data['body'];
    $mailer->AltBody = drupal_html_to_text($data['body']);
    og_mailinglist_add_email_headers($mailer, $node, $group_node);
    _og_mailinglist_add_sender($mailer, $node, $group_node);
    _og_mailinglist_add_recipients($mailer, $node, $group_node, $messaged_users, FALSE);

    // Add attachments if any.
    if (isset($node->files)) {
      foreach ($node->files as $file) {
        $file = (object) $file;
        $mailer->AddAttachment($_SERVER['DOCUMENT_ROOT'] . base_path() .
                               $file->filepath, $file->filename);
      }
    }

    $success = $mailer->Send();
  }
  if ($success) {
    og_mailinglist_log_email_sent('web', $node->nid, 0, $mailer->MessageID, 0, 0, $mailer->MessageID);
  }
  else {
    watchdog('og_mailinglist', "OG_Mailinglist couldn't send a new node email for node.", NULL, WATCHDOG_ERROR);
  }
}

/*
 * Render nodes according to the default input filter.
 *
 * Code taken and modified from the core node_view function.
 *
 * @param $nid
 *  Node ID
 *
 * @return
 *  Node with rendered body.
 */
function og_mailinglist_render_node_body($node) {
  $node = (object) $node;
  $node->build_mode = OG_MAILINGLIST_BUILD_FULL;
  $node = node_build_content($node, FALSE, TRUE);

  // Set the proper node part, then unset unused $node part so that a bad
  // theme can not open a security hole.
  $content = drupal_render($node->content);
  $node->body = $content;
  unset($node->teaser);

  // Allow modules to modify the fully-built node.
  module_invoke_all('node_build_alter', $node);

  return $node;
}

/**
 * Add needed email headers.
 */
function og_mailinglist_add_email_headers($mailer, $node, $group_node, $comment = Null, $in_reply_to = Null, $references = Null) {
  $server = variable_get('og_mailinglist_server_string', $_SERVER['SERVER_NAME']);
  $email_prefix = $group_node->group_email[LANGUAGE_NONE][0]['value'];
  $group_email = $email_prefix . '@' . $server;

  $mailer->AddCustomHeader($mailer->HeaderLine("List-Id", "<" . $group_email . ">"));
  $mailer->AddCustomHeader($mailer->HeaderLine("List-Post", "<mailto:" . $group_email . ">"));
  $mailer->AddCustomHeader($mailer->HeaderLine("List-Archive",
    url("node/" . $group_node->nid, array('absolute' => TRUE))));
  $mailer->AddCustomHeader($mailer->HeaderLine("X-Thread-Url",
    url('node/' . $node->nid)));
  $mailer->AddCustomHeader($mailer->HeaderLine("X-BeenThere", $group_email));

  // Add in-reply-to / references
  if (!empty($in_reply_to)) {
    $mailer->AddCustomHeader($mailer->HeaderLine("In-Reply-To", $in_reply_to));
  }
  if (!empty($references)) {
    $mailer->AddCustomHeader($mailer->HeaderLine("References", $references));
  }

  // Add a reply-to header directed to the group if enabled.
  if (variable_get('og_mailinglist_reply_to_group', FALSE)) {
    $mailer->AddCustomHeader($mailer->HeaderLine("reply-to", "<" . $group_email . ">"));
  }

  // Set Message-ID.
  $comment_id = (!empty($comment)) ? $comment->cid : 0;
  $mailer->MessageID = "<node-" . $node->nid . "-comment-" . $comment_id . "@" . $server . ">";
}

function _og_mailinglist_add_sender($mailer, $node, $group_node, $comment = NULL) {
  if (!empty($comment->uid)) {
    $author = user_load($comment->uid);
  }
  else {
    $author = user_load($node->uid);
  }

  if (isset($author->realname)) {
    $author_name = $author->realname;
  }
  else {
    $author_name = $author->name;
  }

  $server = variable_get('og_mailinglist_server_string', $_SERVER['SERVER_NAME']);
  if ($author->uid == 0) {
    $author->mail = "anonymous@" . $server;
    $author_name = t("anonymous");
  }

  try {
    $mailer->From = $author->mail;
    $mailer->FromName = $author_name;
    $mailer->AddAddress($group_node->group_email . "@" . $server, $group_node->title);
  }
  catch (phpmailerException $e) {
    watchdog('og_mailinglist', t('PHPMailer error: !error', array('!error' => $e->errorMessage())), NULL, WATCHDOG_ERROR);
  }
  catch (Exception $e) {
    watchdog('og_mailinglist', $e->getMessage(), NULL, WATCHDOG_ERROR);
  }
}

function _og_mailinglist_add_recipients($mailer, $node, $group_node, &$messaged_users, $new_node = FALSE) {
  try {
    $subscribers = _og_mailinglist_get_subscribers($node, $group_node, $new_node);
    // Add emails to BCC
    foreach ($subscribers as $uid => $email) {
      if (!in_array($uid, $messaged_users)) {
        $mailer->AddBCC($email);
        $messaged_users[] = $uid;
      }
    }
  }
  catch (phpmailerException $e) {
    watchdog('og_mailinglist', t('PHPMailer error: !error', array('!error' => $e->errorMessage())), NULL, WATCHDOG_ERROR);
  }
  catch (Exception $e) {
    watchdog('og_mailinglist', $e->getMessage(), NULL, WATCHDOG_ERROR);
  }
}

function _og_mailinglist_get_group_emails($group_nodes) {
  $mails = array();
  foreach ($group_nodes as $group_node) {
    $mails[] = $group_node->group_email[LANGUAGE_NONE][0]['value'];
  }
  return $mails;
}
