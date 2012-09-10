<?php
/**
 * @file
 *  Recieves group email, posts as node or comment and sends it out again.
 */

require_once 'og_mailinglist_utilities.inc';
require_once 'og_mailinglist_api.inc';
og_mailinglist_mimeDecode_load_library();
og_mailinglist_phpmailer_load_library();

/**
 * Receive messages POSTed from an external source.
 *
 * This function enables messages to be sent to a Drupal site via POST or some
 * other RFC822 source input (e.g. directly from a mail server).
 *
 * Original version written by Conan Albrecht in March 2009.
 */
function og_mailinglist_post() {
  $group_name = isset($_POST['group_name']) ? $_POST['group_name'] : NULL;
  $message = isset($_POST['message']) ? $_POST['message'] : '';
  $token = isset($_POST['token']) ? $_POST['token'] : '';
  $hash = md5(variable_get('og_mailinglist_incoming_key', '**') . $message);

  if ($hash != $token) {
    watchdog('access denied', 'Authentication error for POST e-mail. Check that you\'ve set the validation string in /admin/config/group/mailinglist and in site_info.php', array(), WATCHDOG_WARNING);
    return drupal_access_denied();
  }

  // If the Maillog module has been set to turn off emails, return.
  if (!variable_get('maillog_send', TRUE)) {
    watchdog('WATCHDOG_INFO', 'This site is using Maillog and has disabled this site from sending emails so this email was not processed.');
    return;
  }

  _og_mailinglist_process_email($message, $group_name);
}

function _og_mailinglist_process_email($raw_email, $group_name = NULL) {
  try {
    $email = array();

    $email['original_email_text'] = $raw_email;

    // Extract all the needed info from the email into a simple array.
    _og_mailinglist_parse_email($email);

    // If the x-BeenThere header is set, this is an email that was sent out and
    // looped back to the server. Reject it.
    if (!empty($email['headers']['x-beenthere'])) {
      exit();
    }

    // If there isn't a message-id (which sometimes happens with spam emails),
    // reject it.
    if (empty($email['headers']['message-id'])) {
      exit();
    }

    if (empty($email['headers']['subject'])) {
      $email['headers']['subject'] = t('(no subject)');
    }

    // Initialize group_names array.
    $group_names = $group_name ? array($group_name => '') : array();
    
    // Add all possible group_names from TO and CC fields.
    $group_names += _og_mailinglist_extract_group_names($email['to']);
    if (isset($email['cc'])) {
      $group_names += _og_mailinglist_extract_group_names($email['cc']);
    }

    // Did we actually get email text back? If not, throw an exception.
    if ($email['text'] == "") {
      throw new Exception(t("Could not parse message body from the email."));
    }

    // Check the size of the body and kick out if too large (for security).
    $max_size = variable_get('og_mailinglist_max_message_size', 200); // 200kb
    if (strlen($email['text']) > $max_size * 1024 && $max_size != 0) {
      throw new Exception(t("Discussion items sent via email must be less than !max_size Kb. For security reasons, please post larger messages through the web interface.", array("!max_size" => $max_size)));
    }

    // Get the user id.
    global $user;
    $user = user_load_by_mail(key($email['from']));

    // If site is using multiple_email, check for alternative emails.
    if (!$user && module_exists('multiple_email') && $multiple = multiple_email_find_address(key($email['from']))) {
      $user = user_load($multiple->uid);
    }

    if (!$user) {
      $exception = t("Could not locate the user account for !mailfrom. For security reasons, please post from the email account you are registered with.", array('!mailfrom' => key($email['from'])));
      throw new Exception($exception);
    }

    // Check how many posts have been made by this user (for security).
    if ($max_posts = variable_get('og_mailinglist_max_posts_per_hour', 20) > 0) {
      $one_hour_ago = REQUEST_TIME - 3600;
      $num_recent_posts = db_select('node', 'n')
        ->condition('n.uid', $user->uid)
        ->condition('n.created', $one_hour_ago, '>')
        ->countQuery()
        ->execute()
        ->fetchField();
      if ($num_recent_posts > $max_posts) {
        throw new Exception(t("You have posted via email too many times in the last hour. For security reasons, please wait a while or post through the regular web interface."));
      }
    }

    // Get the group IDs.
    // We are safe since the group_email field is required to be lowercase.
    $group_names_list = array();
    foreach ($group_names as $group_name => $group_email) {
      $group_names_list[] = strtolower($group_name);
    }
    $query = new EntityFieldQuery;
    $query->entityCondition('entity_type', 'node')
      ->fieldCondition(OG_MAILINGLIST_EMAIL_FIELD, 'value', $group_names_list, 'IN');
    $result = $query->execute();

    if (empty($result)) {
      throw new Exception(t("Could not locate groups with names !group_names",
        array('!group_names' => '"' . implode('", "', $group_names_list) . '"')));
    }

    // Load group nodes.
    $gids = array_keys($result['node']);
    $group_nodes = node_load_multiple($gids);
    $email['group_nodes'] = $group_nodes;

    // Check if the group is published.
    // If it's not published, it's effectively not there.
    foreach ($group_nodes as $gid => $group_node) {
      if ($group_node->status === 0) {
        unset($group_nodes[$gid]);
      }
    }
    if (empty($group_nodes)) {
      throw new Exception(t("None of the groups are published."));
    }

    // Check if this user is a member of these groups (for security).
    foreach ($group_nodes as $gid => $group_node) {
      if (!og_is_member('node', $group_node->nid, 'user', $user)) {
        unset($group_nodes[$gid]);
        // TODO also check if person is subscribed to this thread -- if they are,
        // let them comment by email.
      }
    }

    if (empty($group_nodes)) {
      throw new Exception(t("You (email: !mailfrom, account page: !account_page) are not a member of any of these groups. Please join the group via the web site before posting.",
        array(
          '!mailfrom' => $user->mail,
          '!account_page' => url("user/" . $user->uid, array('absolute' => TRUE)),
        )
      ));
    }

    // Additional check that user has access to publish content
    $node_type = variable_get('og_mailinglist_default_content_type', 'article');
    if (!user_access('create ' . $node_type . ' content', $user)) {
      throw new Exception(t("You do not have permission to post using group content type @type. Please contact the site administrator.",
        array('@type' => $node_type)));
    }

    // See if the email is a comment to an existing node or a new node.
    // If the email is a comment to a node, we should be able to get the
    // node ID of the original node.
    $email['nid'] = og_mailinglist_parse_nid($email);
    // Create the new content in Drupal.
    if (!empty($email['nid'])) { // a new comment
      /* Two checks first before creating the comment.
       * There are at least two reasons why an email could have a nid but not be
       * intended as a new comment.
       * First, someone could be forwarding an email to a different group.
       * Second, it's common on mailinglists to fork threads by changing
       * the subject line. We need to check for both.
       */

      // Does the detected nid belong to any of the groups where the email was
      // forwarded to?
      // If not, then it was intended to start a new thread.
      $direct_members = array();
      $not_direct_members = array();
      foreach ($group_nodes as $gid => $group_node) {
        if (og_is_member('node', $group_node->nid, 'node', $email['nid'])) {
          $direct_members[$gid] = $group_node;
        }
        else {
          $not_direct_members[$gid] = $group_node;
        }
      }
      // TODO When there is at least one direct member and also not direct
      // ones, then we just add the node to other groups if the user is
      // member of these groups (has permission to post there).
      // If there are no direct members, then it is definitely a new
      // post. So it turns out that you can add groups by email but you cannot
      // remove these, so when you reply using any of the addresses to a
      // existing node, everybody is notified.
      if (empty($direct_members) and count($not_direct_members)) {
        og_mailinglist_save_node($email, $group_nodes);
        exit(0); // So we don't save a comment as well
      }

      // Check if the thread was forked by comparing the email subject to the
      // node's title. The levenshtein distance between a reply and node title
      // should be 4 (e.g. "a subject" and "Re: a subject"). I added a few more
      // to the distance to account for oddness in foreign languages but
      // is still small enough to catch any substantive subject change.
      $node = node_load($email['nid']);

      // Remove the group identifier (e.g. [a-group]) from the subject as it's
      // not in the node title.
      $treated_subject = preg_replace("/^[RrEe:]{3}\s\[[\w-]*\]/", "", $email['headers']['subject']);

      if (levenshtein($node->title, $treated_subject) > 7) {
        // The subject was changed so we'll create a new node now instead of a comment.
        // Unset the in-reply-to and references headers so this new thread doesn't get merged with the old thread.
        unset($email['headers']['in-reply-to']);
        unset($email['structure']->headers['in-reply-to']);
        unset($email['headers']['references']);
        unset($email['structure']->headers['references']);
        og_mailinglist_save_node($email, $group_nodes);
        exit(0); // So we don't save a comment as well.
      }

      // If we got this far, the email is definitely intended as a new comment.
      og_mailinglist_save_comment($email, $group_nodes, $node);

    }
    else { // A new node.
      og_mailinglist_save_node($email, $group_nodes);
    }

    // Tell MTA we had success!
    exit(0);

  }
  catch (Exception $e) {
    try {
      $server = variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']);
      $noreply = variable_get("og_mailinglist_noreply_email", t("no-reply@") . $server);
      // Compose an email back to the sender informing them of the problem.
      $head = array();
      $head[] = 'From: ' . $noreply;
      $head[] = 'References: ' . $email['headers']['message-id'];
      $head[] = 'In-Reply-To: ' . $email['headers']['message-id'];
      $head[] = 'X-BeenThere: ' . $server;
      $errormsg = $e->getMessage();
      $msgdate = $email['headers']['date'];
      $commentedbody = '> ' . str_replace("\n", "\n> ", $email['text']);
      $body = t("Hi, sorry about this but we were unable to handle your email correctly for the following reason:")
        . "\n\n"
        . $errormsg
        . "\n\n"
        . t("Please correct this error and try again, or contact the system administrator.  Thank you.")
        . "\n\n---- " . t("Below this line is a copy of the message") . " ----\n"
        . $commentedbody;

      // Send it off.
      $from = _og_mailinglist_compose_recipients($email['from'], TRUE);
      if (!mail($from, t("Error processing message"), $body, implode("\n", $head))) {
        throw new Exception("Mail error");
      }

      // print error message to log, then quit
      echo "Error: " . $e->getMessage() . "\n";
      exit(0);

    }
    catch (Exception $e2) {
      // If we got here, we couldn't even send an email back to the sender, so
      // just have MTA compose an error message and send it back.
      echo "Error: " . $e2->getMessage() . " ::: Embedded Error: " . $e->getMessage() . "\n";
      exit(1);
    }
  }
}

/**
 * Save new comment that came through email.
 */
function og_mailinglist_save_comment(&$email, &$group_nodes, $node) {
  global $user;
  $email['node'] = $node;

  // Let other modules alter the new comment or delay posting for another time
  // (e.g. a moderation module might want to delay posting for a time).
  drupal_alter('og_mailinglist_new_email_comment', $email);

  // Other modules can unset certain emails to stop them from being saved and sent.
  // If they do, we'll just exit out.
  if (empty($email)) {
    return;
  }

  // check that this user has rights to post comments
  if (!user_access('post comments')) {
    throw new Exception(t("You do not have rights to post comments in the system."));
  }

  // Check that this post has comments enabled.
  if ($node->comment != COMMENT_NODE_OPEN) {
    throw new Exception(t("Comments (replies) have been disabled for this post."));
  }

  $mailbody = $email['text'];

  // Create an object representing the comment
  $comment = new stdClass();
  $comment->cid = 0;
  $comment->nid = $node->nid;
  $comment->uid = $user->uid;
  $comment->language = LANGUAGE_NONE;
  $comment->is_anonymous = 0;
  $comment->node_type = 'comment_node_' . $node->type;

  // A custom variable to keep phpmailer from sending double email.
  $comment->og_mailinglist_from_email = TRUE;

  if (preg_match("/re:\s*\[.*?\]\s*(.*)/i", $email['headers']['subject'], $matches)) {
    $comment->subject = $matches[1];
  }
  elseif (preg_match("/re: +(.*)/i", $email['headers']['subject'], $matches)) {
    $comment->subject = $matches[1];
  }
  else {
    $comment->subject = $email['headers']['subject'];
  }

  $comment->comment_body[$comment->language][0]['value'] = $mailbody;
  $filter = variable_get('og_mailinglist_default_filter_format', 'plain_text');
  $comment->comment_body[$comment->language][0]['format'] = $filter;

  // Log that comment came from email so og_mailinglist_phpmailer doesn't send an email as well.
  $parent_message_id = _og_mailinglist_get_thread_parent_messageid($email['headers']);

  // If enabled, set comment parent = cid of in-reply-to for comment threading
  if (variable_get('og_mailinglist_thread_comments', FALSE)) {
    $comment->thread = '01/';
    $comment->pid = db_query("SELECT cid FROM {og_mailinglist_source} WHERE message_id = :message_id", array(':message_id' => $email['headers']['in-reply-to']))->fetchField();
  }
  else {
    $comment->pid = 0;
  }

  // Let other modules alter the comment object before saving.
  drupal_alter('og_mailinglist_save_comment', $comment);

  // Save the new comment.
  comment_submit($comment);
  comment_save($comment);

  _og_mailinglist_email_comment_email($email, $node, $group_nodes, $comment);

  $in_reply_to = isset($email['headers']['in-reply-to']) ? $email['headers']['in-reply-to'] : '';
  $references = isset($email['headers']['references']) ? $email['headers']['references'] : '';

  og_mailinglist_log_email_sent('email', $node->nid, $comment->cid, $email['headers']['message-id'], $in_reply_to, $references, $parent_message_id);

  // Report activity to watchdog.
  $group_list = array();
  foreach ($group_nodes as $gid => $group_node) {
    $group_list[] = $group_node->title . ' (gid' . $gid . ')';
  }
  watchdog('og_mailinglist', "Posted comment for @mailfrom as cid=@cid to nid=@nid inside groups: @group_list",
    array(
      '@mailfrom' => $user->name,
      '@group_list' => implode($group_list, ', '),
      '@nid' => $node->nid,
      '@cid' => $comment->cid,
    )
  );
}

/**
 * Save new nodes that came through email.
 */
function og_mailinglist_save_node(&$email, &$group_nodes) {
  global $user;

  // Let other modules alter the new node or delay posting for another time
  // (e.g. a moderation module might want to delay posting for a time).
  drupal_alter('og_mailinglist_new_email_node', $email);

  // Other modules can unset certain emails to stop them from being saved and sent.
  // If they do, we'll just exit out.
  if (empty($email)) {
    exit(0);
  }

  $mailbody = $email['text'];

  // Is the group public? If not, we need to set the node as private.
  // The og_private table stores a 1 if the group is private and 0 if it's public.
  // node_save() expects the opposite so we'll invert the value of $is_private.
  // TODO: Check whether it actually works.
  $is_public = TRUE;
  if (defined("OG_CONTENT_ACCESS_FIELD")) {
    // TODO: if possible, get rid of node_load() here
    $access = field_get_items('node', node_load($email['groupid']), OG_CONTENT_ACCESS_FIELD);
    // TODO: There is also OG_CONTENT_ACCESS_DEFAULT case.
    if ($access == OG_CONTENT_ACCESS_PRIVATE) {
      $is_public = FALSE;
    }
  }

  // Create the new node
  $node = new stdClass();
  $node->type = variable_get('og_mailinglist_default_content_type', 'article');
  node_object_prepare($node);

  $node->title = $email['headers']['subject'];
  $node->uid = $user->uid;
  $node->language = LANGUAGE_NONE;
  $node->body[$node->language][0]['value'] = $mailbody;
  $filter = variable_get('og_mailinglist_default_filter_format', 'plain_text');
  $node->body[$node->language][0]['format'] = $filter;

  // Custom value to keep the e-mail from being sent twice.
  $node->og_mailinglist_from_email = TRUE;

  // TODO: This needs love.
  //$node->og_public = $is_public;
  foreach ($group_nodes as $gid => $group_node) {
    $node->og_group_ref[$node->language][]['target_id'] = $gid;
  }

  // Add attachments if any.
  // TODO fix this someday. Best idea -- save mail objects w/ attachments.
  // On cron scoop them up and add them to nodes/comments
  //if (isset($email['attachments'])) {
  //  $nodeattachments = _og_mailinglist_save_attachments_temp_dir($email['attachments']);
  //  $node->og_mailinglist_attachments = $nodeattachments;
  //  _og_mailinglist_save_files($node);
  //}

  // Let other modules alter the node object before saving.
  drupal_alter('og_mailinglist_save_node', $node);

  node_save($node);

  // Log that this comment came from email.
  og_mailinglist_log_email_sent('email', $node->nid, 0, $email['headers']['message-id'], 0, 0, $email['headers']['message-id']);

  // Send off email.
  _og_mailinglist_email_node_email($email, $node, $group_nodes);

  $group_list = array();
  foreach ($group_nodes as $gid => $group_node) {
    $group_list[] = $group_node->title . ' (gid' . $gid . ')';
  }
  // Report activity to watchdog.
  watchdog('og_mailinglist', 'Posted node @title (@nid) for @username (@email) to groups: @group_list.',
    array(
      '@title' => $node->title,
      '@nid' => $node->nid,
      '@username' => $user->name,
      '@email' => $user->mail,
      '@group_list' => implode($group_list, ', '),
    )
  );
}

function _og_mailinglist_email_node_email(&$email, &$node, &$group_nodes) {
  // Build new email.
  _og_mailinglist_rewrite_headers($email, $group_nodes, $node, TRUE);

  $server = variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']);
  $subject_start = '';
  foreach ($group_nodes as $group_node) {
    $group_name = _og_mailinglist_get_group_name($group_node);
    $group_email = $group_name . '@' . $server;
    $subject_start .= '[' . $group_name . ']';
  }
  $email['headers']['subject'] = $subject_start . ' ' . $node->title;

  $footer = _og_mailinglist_build_footer($node);
  $email = _og_mailinglist_add_footer($email, $footer);
  $headers = $email['structure']->headers;
  unset($email['structure']->headers);
  $email['new_email_text'] = _og_mailinglist_encode_email(array($email['structure']));

  // Check if entire body should be encoded as base64
  $email['new_email_text'] = _og_mailinglist_check_base64($email['new_email_text'], $headers);

  // Send it off.
  _og_mailinglist_send_email($headers, $email['new_email_text'], $node, NULL);

  // If the sender's subscription type isn't email, give him a thread subscription.
  if (og_mailinglist_get_group_subscription_type($node->og_group_ref[$node->language][0], $node->uid) != "email") {
    og_mailinglist_save_thread_subscriptions($node->nid, array($node->uid));
  }
}

function _og_mailinglist_email_comment_email($email, $node, $group_nodes, $comment) {
  // Build new email.
  _og_mailinglist_rewrite_headers($email, $group_nodes, $node);

  $footer = _og_mailinglist_build_footer($node);
  $email = _og_mailinglist_add_footer($email, $footer);
  $headers = $email['headers'];
  unset($email['structure']->headers);
  $email['new_email_text'] = _og_mailinglist_encode_email(array($email['structure']));

  // Check if entire body should be encoded as base64
  $email['new_email_text'] = _og_mailinglist_check_base64($email['new_email_text'], $headers);

  // Send it off.
  _og_mailinglist_send_email($headers, $email['new_email_text'], $node, $comment);
}

function _og_mailinglist_parse_email(&$email) {
  $params = array(
    'include_bodies' => TRUE,
    'decode_bodies' => TRUE,
    'decode_headers' => TRUE,
  );

  // Decode the email message.
  $decoder = new Mail_mimeDecode($email['original_email_text']);
  $structure = $decoder->decode($params);
  $email['structure'] = clone $structure;

  // Copy headers to the $email array.
  $email['headers'] = array_copy($structure->headers);

  if (!empty($structure->parts)) {
    foreach ($structure->parts as $i => $part) {
      if (!empty($part->parts)) {
        $sub_part = $part->parts;

        foreach ($sub_part as $j => $sub_part_part) {
          if (isset($sub_part_part->disposition) and ($sub_part_part->disposition === 'attachment')) {
            $email['attachments'][] = _og_mailinglist_parse_attachment($sub_part_part);
          }
          elseif ($sub_part_part->ctype_secondary == 'plain') {
            $email['text'] = _og_mailinglist_parse_content($sub_part_part);
          }
          elseif ($sub_part_part->ctype_secondary == 'html') {
            $email['html'] = _og_mailinglist_parse_content($sub_part_part);
          }
        }
      }

      if (isset($part->disposition) and ($part->disposition === 'attachment')) {
        $email['attachments'][] = _og_mailinglist_parse_attachment($part);
      }
      elseif (($part->ctype_secondary == 'plain') && empty($email['text'])) {
        $email['text'] = _og_mailinglist_parse_content($part);
      }
      elseif (($part->ctype_secondary == 'html') && empty($email['html'])) {
        $email['html'] = _og_mailinglist_parse_content($part);
      }
    }
  }
  // Make sure we always have text.
  if (isset($email['html']) && !isset($email['text'])) {
    $email['text'] = preg_replace('|<style.*</style>|mis', '', $email['html']);
    $email['text'] = drupal_html_to_text($email['text']);
  }

  // Set the default character encoding.
  $charset = 'iso-8859-1';

  if (!empty($structure->ctype_parameters['charset'])) {
    $charset = $structure->ctype_parameters['charset'];
  }

  // Last ditch attempt - use the body as-is.
  if (!isset($email['text'])) {
    // Try to convert to UTF-8.
    $email['text'] = drupal_convert_to_utf8($structure->body, $charset);
  }

  // Convert the subject and recipient fields to UTF-8.
  if (isset($email['headers']['subject'])) {
    $email['headers']['subject'] = drupal_convert_to_utf8($email['headers']['subject'], $charset);
  }

  $raw_emails = drupal_convert_to_utf8($email['headers']['from'], $charset);
  $email['from'] = _og_mailinglist_extract_emails($raw_emails);
  unset($email['headers']['from']);

  $raw_emails = drupal_convert_to_utf8($email['headers']['to'], $charset);
  $email['to'] = _og_mailinglist_extract_emails($raw_emails);
  unset($email['headers']['to']);

  if (isset($email['headers']['cc'])) {
    $raw_emails = drupal_convert_to_utf8($email['headers']['cc'], $charset);
    $email['cc'] = _og_mailinglist_extract_emails($raw_emails);
    unset($email['headers']['cc']);
  }
}
// TODO fix attachments for filefield

function _og_mailinglist_parse_attachment($part) {
  $info['data'] = $part->body;
  $info['filemime'] = $part->ctype_primary . "/" . $part->ctype_secondary;
  $info['filename'] = $part->ctype_parameters['name'];

  return $info;
}

function _og_mailinglist_parse_content($part) {
  $content = $part->body;

  // Try to convert character set to UTF-8.
  if (!empty($part->ctype_parameters['charset'])) {
    $content = drupal_convert_to_utf8($content, $part->ctype_parameters['charset']);
  }

  return $content;
}

function _og_mailinglist_save_files(&$node) {
  global $user;

  // If $node->og_mailinglist_attachments is empty or upload not installed just return
  if (!$node->og_mailinglist_attachments || !module_exists('upload')) {
    return;
  }

  // If user doesn't have upload permission then don't bother processing
  if (!(user_access('upload files'))) {
    //echo "didn't have permissions?\n\n";
    return;
  }

  // Convert $node->og_mailinglist_attachments to $node->files ready for upload to use
  foreach ($node->og_mailinglist_attachments as $filekey => $attachment) {

    $limits = _upload_file_limits($user);
    $validators = array(
      'file_validate_extensions' => array($limits['extensions']),
      'file_validate_image_resolution' => array($limits['resolution']),
      'file_validate_size' => array($limits['file_size'], $limits['user_size']),
    );

    if ($file = _og_mailinglist_save_file($attachment, $validators)) {
      // Create the $node->files elements
      $file->list = variable_get('upload_list_default', 1);
      $file->description = $file->filename;
      $node->files[$file->fid] = $file;

      // This is a temporary line to get upload_save to work (see upload.module line 413)
      // upload_save checks for either the presence of an old_vid, or the session variable, to determine
      // if a new upload has been supplied and create a new entry in the database
      $node->old_vid = 1;
    }

  }

  // Destroy $node->og_mailinglist_attachments now we have created $node->files
  unset($node->og_mailinglist_attachments);

}


// This started as a copy of file_save_upload
//function _og_mailinglist_node_file($attachment, $source, $validators = array(), $dest = FALSE, $replace = FILE_EXISTS_RENAME) {
function _og_mailinglist_save_file($attachment, $validators = array()) {
  global $user;

  // Add in our check of the the file name length.
  $validators['file_validate_name_length'] = array();

  // Build the list of non-munged extensions.
  // @todo: this should not be here. we need to figure out the right place.
  $extensions = '';
  foreach ($user->roles as $rid => $name) {
    $extensions .= ' ' . variable_get("upload_extensions_$rid",
    variable_get('upload_extensions_default', 'jpg jpeg gif png txt html doc xls pdf ppt pps odt ods odp'));
  }

  // Begin building file object.
  $file = new stdClass();
  $file->filename = file_munge_filename(trim(basename($attachment['filename']), '.'), $extensions);
  $file->filepath = $attachment['filepath'];
  $file->filemime = file_get_mimetype($file->filename);

  // Rename potentially executable files, to help prevent exploits.
  if (preg_match('/\.(php|pl|py|cgi|asp|js)$/i', $file->filename) && (substr($file->filename, -4) != '.txt')) {
    $file->filemime = 'text/plain';
    $file->filepath .= '.txt';
    $file->filename .= '.txt';
  }

  // Create temporary name/path for newly uploaded files.
  //if (!$dest) {
  $dest = file_destination(file_create_path($file->filename), FILE_EXISTS_RENAME);
  //}
  //$file->source = $source;
  $file->destination = $dest;
  $file->filesize = $attachment['filesize'];

  // Call the validation functions.
  $errors = array();
  foreach ($validators as $function => $args) {
    array_unshift($args, $file);
    $errors = array_merge($errors, call_user_func_array($function, $args));
  }

  // Check for validation errors.
  if (!empty($errors)) {
    watchdog('mailhandler', 'The selected file %name could not be uploaded.', array('%name' => $file->filename), WATCHDOG_WARNING);
    while ($errors) {
      watchdog('mailhandler', array_shift($errors));
    }
    return 0;
  }

  // Move uploaded files from PHP's tmp_dir to Drupal's temporary directory.
  // This overcomes open_basedir restrictions for future file operations.
  $file->filepath = $file->destination;
  if (!file_move($attachment['filepath'], $file->filepath)) {
    watchdog('mailhandler', 'Upload error. Could not move file %file to destination %destination.', array('%file' => $file->filename, '%destination' => $file->filepath), WATCHDOG_ERROR);
    return 0;
  }

  // If we made it this far it's safe to record this file in the database.
  $file->uid = $user->uid;
  $file->status = ~FILE_STATUS_PERMANENT;
  $file->timestamp = REQUEST_TIME;
  drupal_write_record('files', $file);

  // Return the results of the save operation
  return $file;

}

function _og_mailinglist_save_attachments_temp_dir($attachments) {
  // Parse each mime part in turn
  foreach ($attachments as $info) {
    // Save the data to temporary file
    $temp_file = tempnam(file_directory_temp(), 'mail');
    $filepath = file_save_data($info['data'], $temp_file);

    // Add the item to the attachments array, and sanitise filename
    $node_attachments[] = array(
      'filename' => _og_mailinglist_sanitise_filename($info['filename']),
      'filepath' => $filepath,
      'filemime' => strtolower($info['filemime']),
      'filesize' => strlen($info['data']),
    );
  }
  file_save_data("hello world", file_directory_path() . "/temp");

  // Return the attachments
  return $node_attachments;

}

/**
 * Take a raw attachment filename, decode and strip out invalid characters.
 *
 * @return
 *   A sanitised filename that should be ok for use by modules that want
 *   to save the file.
 */
function _og_mailinglist_sanitise_filename($filename) {
  // Decode multibyte encoded filename
  $filename = mb_decode_mimeheader($filename);

  // Replaces all characters up through space and all past ~ along with the
  // above reserved characters to sanitise filename
  // from php.net/manual/en/function.preg-replace.php#80431

  // Define characters that are  illegal on any of the 3 major OS's
  $reserved = preg_quote('\/:*?"<>|', '/');

  // Perform cleanup
  $filename = preg_replace("/([\\x00-\\x20\\x7f-\\xff{$reserved}])/e", "_", $filename);

  // Return the cleaned up filename
  return $filename;
}

// Turn structure back into a plain text email using recursion.
function _og_mailinglist_encode_email($structure, $boundary = "", $email = "") {
  foreach ($structure as $part) {
    if (empty($boundary) and isset($part->ctype_parameters['boundary'])) {
      $boundary = $part->ctype_parameters['boundary'];
    }

    if (isset($part->parts)) {
      if (isset($part->headers)) {
        $email .= _og_mailinglist_encode_email_headers($part->headers) . "\n";
      }
      $email .= "--" . $part->ctype_parameters['boundary'] . "\n";
      $email = _og_mailinglist_encode_email($part->parts, $part->ctype_parameters['boundary'], $email);
      $email .= "--" . $part->ctype_parameters['boundary'] . "--\n";
    }
    else {
      // Non-multipart emails don't have boundaries
      if (isset($boundary) and $boundary) {
        $last_line = substr(strrchr(trim($email), "\n"), 1);
        if (strcmp(trim($last_line), trim("--" . $boundary)) != 0) {
          $email .= "--" . $boundary . "\n";
        }
      }

      if (isset($part->headers)) {
        $email .= _og_mailinglist_encode_email_headers($part->headers) . "\n";
      }
      // Encode the body as base64 if necessary
      if (isset($part->headers['content-transfer-encoding']) and
        $part->headers['content-transfer-encoding'] == "base64") {
        $email .= wordwrap(base64_encode($part->body), 76, "\n", TRUE);
        $email .= "\n";
      }
      elseif (isset($part->body)) {
        $email .= $part->body . "\n";
      }
    }
  }

  return $email;
}

function _og_mailinglist_encode_email_headers($array) {
  $header = "";
  if (empty($array)) {
    return $header;
  }
  foreach ($array as $key => $value) {
    // We remove quoted-printable as content-transfer-encoding
    // because mime_decode decodes that and PHP doesn't have a function
    // AFAIK to reencode the text as quoted-printable.
    if ($value && $value !== "quoted-printable") {
      $header .= capitalize_headers($key, " -") . ": " . $value . "\n";
    }
  }

  return $header;
}

// Keep mime-version, date, subject, from, to, and content-type
function _og_mailinglist_rewrite_headers(&$email, &$group_nodes, &$node, $new_node = FALSE) {
  $headers = array_copy($email['headers']);

  // Header values which we want to keep. All lowercase here.
  $copy_headers = array('content-type', 'content-transfer-encoding',
    'date', 'from', 'in-reply-to', 'message-id', 'mime-version', 'references',
    'subject');

  // Let's delete unnecessary header fields.
  foreach ($headers as $key => $value) {
    if (!in_array(strtolower($key), $copy_headers)) {
      unset($headers[$key]);
    }
  }

  // TODO: This one here looks like a slight hack.
  $group_node = reset($group_nodes);

  $group_name = _og_mailinglist_get_group_name($group_node);
  $server = variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']);
  $group_email = $group_name . '@' . $server;

  $headers['to'] = $group_email;
  $headers['bcc'] = _og_mailinglist_get_subscribers($node, $group_node, $new_node);

  // Add a reply-to header directed to the group if enabled.
  if (variable_get('og_mailinglist_reply_to_group', FALSE)) {
    $headers['reply-to'] = $group_email;
  }

  // Group node URL
  $url = url("node/" . $group_node->nid, array('absolute' => TRUE));

  // Add list headers.
  $headers['List-Id'] = "<" . $group_email . ">";
  $headers['List-Post'] = "<mailto:" . $group_email . ">";
  $headers['List-Archive'] = $url;

  // Thread-URL header.
  $headers['X-Thread-Url'] = $url;

  // Set x-BeenThere header
  $headers['X-BeenThere'] = $group_email;

  $email['headers'] = $headers;
}

function _og_mailinglist_check_base64($body, $headers) {
  // If text/plain + base64
  if (isset($headers['content-transfer-encoding']) and
    $headers['content-transfer-encoding'] == 'base64') {
    return wordwrap(base64_encode($body), 76, "\n", TRUE) . "\n";
  }
  return $body;
}

function _og_mailinglist_add_footer($email, $footer) {
  $headers = $email['headers'];
  $structure = $email['structure'];

  // If message is 7/8bit text/plain and uses us-ascii charecter set, just
  // append the footer.
  if (preg_match('/^text\/plain/i', $headers['content-type']) &&
      isset($structure->body)) {
    $structure->body .= "\n" . $footer;
  }
  // If message is already multipart, append a new part with footer to the end.
  elseif (preg_match('/^multipart\/(mixed|related)/i', $headers['content-type'])
             && isset($structure->parts)) {
    $structure->parts[] = (object) array(
      "headers" => array(
        "content-type" => 'text/plain; charset="us-ascii"',
        "mime-version" => '1.0',
        "content-transfer-encoding" => '7bit',
        "content-disposition" => 'inline',
      ),
      "ctype_primary" => 'text',
      "ctype_secondary" => 'plain',
      "ctype_parameters" => array(
        "charset" => 'us-ascii',
      ),
      "disposition" => 'inline',
      "body" => $footer,
    );
  }
  else {
    // Else, move existing fields into new MIME entity surrounded by new multipart
    // and append footer field to the end.
    $structure->headers['mime-version'] = "1.0";
    $boundary = "Drupal-OG-Mailinglist--" . rand(100000000, 9999999999999);

    // Copy email, remove headers from copy, rewrite the content-type, add
    // email copy as parts.
    $content_type = $structure->headers['content-type'];
    $str_clone = clone $structure;
    $str_clone->headers = array('content-type' => $content_type);

    $structure->headers['content-type'] = "multipart/mixed; boundary=\"" .
        $boundary . "\"";
    $structure->ctype_primary = "multipart";
    $structure->ctype_secondary = "mixed";
    $structure->ctype_parameters = array('boundary' => $boundary);
    $structure->parts = array($str_clone);
    $structure->parts[] = (object) array(
      "headers" => array(
        "content-type" => 'text/plain; charset="us-ascii"',
        "mime-version" => '1.0',
        "content-transfer-encoding" => '7bit',
        "content-disposition" => 'inline',
      ),
      "ctype_primary" => 'text',
      "ctype_secondary" => 'plain',
      "ctype_parameters" => array(
        "charset" => 'us-ascii',
      ),
      "disposition" => 'inline',
      "body" => $footer,
    );
  }

  $email['structure'] = $structure;

  return $email;
}

function _og_mailinglist_send_email($headers, $body, $node, $comment = NULL) {
  global $user;

  $mailer = og_mailinglist_create_mailer();
  $mailer->ClearAddresses();
  foreach ($headers as $key => $value) {
    if (empty($value)) {
      continue;
    }
    switch (strtolower($key)) {
      case 'date':
        $mailer->MessageDate = $value;
        break;
      case 'message-id':
        $mailer->MessageID = $value;
        break;
      case 'subject':
        $mailer->Subject = $value;
        break;
      case 'to':
        if (preg_match("/([^<]*)<(.+)>/", $value, $matches)) {
          print_r($matches);
          $mailer->AddAddress($matches[2], $matches[1]);
        }
        else {
          $mailer->AddAddress($value);
        }
        break;
      case 'cc':
        if (preg_match("/([^<]*)<(.+)>/", $value, $matches)) {
          $mailer->AddCC($matches[2], $matches[1]);
        }
        else {
          $mailer->AddCC($value);
        }
        break;
      case 'bcc':
        foreach ($value as $recipient) {
          $mailer->AddBCC($recipient);
        }
        break;
      case 'from':
        if (preg_match("/([^<]*)<(.+)>/", $value, $matches)) {
          $mailer->From = $matches[2];
          $mailer->FromName = $matches[1];
        }
        else {
          $mailer->From = $value;
          $mailer->FromName = ($user->realname) ? $user->realname : $user->name;
        }
        break;
      case 'content-type':
        $mailer->ContentType = $value;

        // Parse out the charset if one is set.
        if (preg_match("/charset=(.*)/s", $value, $matches)) {
          $mailer->CharSet = $matches[1];
        }
        break;
      case 'received':
        // An array of lines of where the email has been.
        // I think it is okay to drop it.
        break;
      default:
        $mailer->AddCustomHeader($key . ':' . $value);
    }
  }

  $body = trim($body);
  $mailer->Body = $body;

  $success = $mailer->Send();

  if (!$success) {
    watchdog('og_mailinglist', "OG_Mailinglist couldn't send an email.", array(),
             WATCHDOG_ERROR);
  }

}

/**
 * Try to parse nid from the email.
 *
 * @param $headers
 *  Headers of the email to be parsed.
 */
function og_mailinglist_parse_nid($email) {
  $nid = "";
  // Check the in-reply-to header.
  $parent_message_id = _og_mailinglist_get_thread_parent_messageid($email['headers']);
  if (!empty($parent_message_id)) {
    $nid = _og_mailinglist_get_nid_from_parent_messageid($parent_message_id);
  }

  if ($nid) {
    return $nid;
  }

  $nid = _og_mailinglist_get_nid_from_subject($email['headers']['subject']);
  return $nid;
}

function _og_mailinglist_get_nid_from_subject($subject) {
  $nid = NULL;
  preg_match("/(Re:\s\[.*?\])?\s(.+)/si", $subject, $matches);
  if (isset($matches[2])) {
    $nid = db_query("SELECT nid
      FROM {node}
      WHERE title LIKE :title
      ORDER BY created desc
      LIMIT 1",
      array(':title' => '%' . $matches[2] . '%')
    )->fetchField();
  }
  return $nid;
}

/**
 * Extract email addresses and names from a comma separated string.
 *
 * @param $addresses
 *   "@param" is a comma separated string of email addresses as it is found in
 *   email TO or CC fields, e.g John Smith <john@example.com>, joe@example.com
 *
 * @return
 *   "@return" return an associative array of emails as keys and names as
 *   values. Using keys makes it impossible to have duplicates.
 */
function _og_mailinglist_extract_emails($addresses) {
  $emails = array();
  foreach (explode(',', $addresses) as $email) {
    if (preg_match("@([^<]*)<(.*)>@is", $email, $matches)) {
      $name = trim($matches[1], "\"' ");
      $address = trim($matches[2]);
      $emails[$address] = $name;
    }
    else {
      $emails[trim($email)] = '';
    }
  }
  return $emails;
}

/**
 * Extract group names from an array of email addresses as keys.
 *
 * @param $addresses
 *   "@param" is an array of email addresses, where addresses are held as keys.
 *   This is what is returned from _og_mailinglist_extract_emails().
 *   E.g array('john@example.com' => 'John Smith', 'joe@example.com' => '')
 *
 * @return
 *   "@return" return an associative array of usernames as keys and emails as
 *   values.
 */
function _og_mailinglist_extract_group_names(&$addresses) {
  // Regex borrowed from valid_email_address().
  // Case insensitivity is needed because in domain names case does not matter.
  $server = variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']);
  $regex = "/([a-z0-9_\-\.\+\^!#\$%&*+\/\=\?\`\|\{\}~\']+)@" . $server . "/i";

  $out = array();
  foreach ($addresses as $email => $name) {
    // Parse the group name from the email.
    if (preg_match($regex, $email, $matches)) {
      $out[$matches[1]] = $email;
    }
  }
  return $out;
}

/*
 * Compose recipients string from an array of recipients.
 */
function _og_mailinglist_compose_recipients($recipients, $first_only = FALSE) {
  $out = array();
  foreach ($recipients as $mail => $username) {
    if ($username) {
      $out[] = $username . ' <' . $mail . '>';
    }
    else {
      $out[] = '<' . $mail . '>';
    }
    if ($first_only) {
      break;
    }
  }
  return implode($out, ', ');
}
