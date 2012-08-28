<?php
/**
 * @file
 *  Sends and receives emails.
 */

require_once 'og_mailinglist_utilities.inc';
require_once 'og_mailinglist_api.inc';
og_mailinglist_mimeDecode_load_library();
og_mailinglist_phpmailer_load_library();

/**
 * Receive messages POSTed from an external source.
 *
 * This function enables messages to be sent to a Drupal site via POST or some other RFC822
 * source input (e.g. directly from a mail server).
 *
 * Original version written by Conan Albrecht  March 2009.
 */
function og_mailinglist_post() {
  $message = (isset($_POST['message'])) ? $_POST['message'] : '';
  $token = (isset($_POST['token'])) ? $_POST['token'] : '';
  $group_name = (isset($_POST['group_name'])) ? $_POST['group_name'] : '';

  $hash = md5(variable_get('og_mailinglist_incoming_key', '**') . $message);
  if ($hash != $token) {
    watchdog('access denied', 'Authentication error for POST e-mail. Check that you\'ve set the validation string in /admin/config/group/mailinglist and in site_info.php', array(), WATCHDOG_WARNING);
    return drupal_access_denied();
  }
  // If this the Maillog module has been set to turn off emails, return.
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
    $email = _og_mailinglist_parse_email($email);

    // If the x-BeenThere header is set, this is an email that was sent out and
    // looped back to the server. Reject it.
    if (!empty($email['headers']['x-beenthere'])) {
      exit();
    }

    // If there isn't a message-id (which sometimes happens with spam emails), reject it.
    if (empty($email['headers']['message-id'])) {
      exit();
    }

    if (empty($email['headers']['subject'])) {
      $email['headers']['subject'] = t('(no subject)');
    }

    // Parse the group name from the email. Regex borrowed from valid_email_address().
    $server = variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']);
    if (empty($group_name)) {
      if (preg_match("/([a-zA-Z0-9_\-\.\+\^!#\$%&*+\/\=\?\`\|\{\}~\']+)@" . $server . "/", $email['headers']['to'], $matches)) {
        $group_name = $matches[1];
      }
      if (preg_match("/([a-zA-Z0-9_\-\.\+\^!#\$%&*+\/\=\?\`\|\{\}~\']+)@" . $server . "/", $email['headers']['cc'], $matches)) {
        $group_name = $matches[1];
      }
    }

    // Strip enclosing quotes sometimes added by MS Outlook.
    $group_name = trim($group_name, '"\' ');

    // Did we actually get email text back? If not, throw an exception.
    if ($email['mailbody'] == "") {
      throw new Exception(t("Could not parse message body from the text/plain portion of the email."));
    }

    // Check the size of the body and kick out if too large (for security).
    $max_size = variable_get('og_mailinglist_max_message_size', 200); // 200kb
    if (strlen($email['mailbody']) > $max_size * 1024 && $max_size != 0) {
      throw new Exception(t("Discussion items sent via email must be less than !max_size Kb. For security reasons, please post larger messages through the web interface.", array("!max_size" => $max_size)));
    }

    // Get clean email address.
    $mailfrom = $email['headers']['from'];
    if (preg_match("/<(.*?)>/", $email['headers']['from'], $matches)) {
      $mailfrom = $matches[1];
    }
    else {
      // This regex is borrowed from valid_email_address for D6.
      // This is only needed for these email clients which do not enclose
      // email address between <> tags. See http://drupal.org/node/1039876
      // for example "From: user@example.com (User name)". It would
      // handle any other case as well.
      $user = '[a-zA-Z0-9_\-\.\+\^!#\$%&*+\/\=\?\`\|\{\}~\']+';
      $domain = '(?:(?:[a-zA-Z0-9]|[a-zA-Z0-9][a-zA-Z0-9\-]*[a-zA-Z0-9])\.?)+';
      $ipv4 = '[0-9]{1,3}(\.[0-9]{1,3}){3}';
      $ipv6 = '[0-9a-fA-F]{1,4}(\:[0-9a-fA-F]{1,4}){7}';
      if (preg_match("/$user@($domain|(\[($ipv4|$ipv6)\]))/", $mailfrom, $matches)) {
        $mailfrom = $matches[0];
      }
    }

    // Get the user id.
    $user = user_load_by_mail($mailfrom);
    if ($user) {
      $email['userid'] = $user->uid;
    }

    // If site is using multiple_email, check for alternative emails.
    if (!$email['userid'] && module_exists('multiple_email') && $multiple = multiple_email_find_address($mailfrom)) {
      $email['userid'] = $multiple->uid;
    }

    if (!$email['userid']) {
      $exception = t("Could not locate the user account for !mailfrom. For security reasons, please post from the email account you are registered with.", array('!mailfrom' => $mailfrom));
      throw new Exception($exception);
    }
    // Check how many posts have been made by this user (for security).
    if ($max_posts = variable_get('og_mailinglist_max_posts_per_hour', 20) > 0) {
      $one_hour_ago = REQUEST_TIME - 3600;
      $num_recent_posts = db_select('node', 'n')
        ->condition('n.uid', $email['userid'])
        ->condition('n.created', $one_hour_ago, '>')
        ->countQuery()
        ->execute()
        ->fetchField();
      if ($num_recent_posts > $max_posts) {
        throw new Exception(t("You have posted via email too many times in the last hour. For security reasons, please wait a while or post through the regular web interface."));
      }
    }

    // Get the group id.
    // We are safe since the group_email field is required to be lowercase.
    $query = new EntityFieldQuery;
    $query->entityCondition('entity_type', 'node')
      ->fieldCondition(OG_MAILINGLIST_EMAIL_FIELD, 'value', strtolower($group_name));
    $result = $query->execute();
    $email['groupid'] = key($result['node']);

    if (!$email['groupid']) {
      throw new Exception(t("Could not locate group named !group_name",
        array('!group_name' => $group_name)));
    }

    // Check that the group is published. If it's not published, it's
    // effectively not there.
    $group_node = node_load($email['groupid']);
    if ($group_node->status === 0) {
      throw new Exception(t("Could not locate group named !group_name",
        array('!group_name' => $group_name)));
    }

    // Check if this user is a member of this group (for security).
    if (!og_is_member('node', $email['groupid'], 'user', $email['userid'])) {
      // TODO also check if person is subscribed to this thread -- if they are,
      // let them comment by email.
      throw new Exception(t("You (email: !mailfrom, account page: !account_page) are not a member of this group.  Please join the group via the web site before posting.",
        array(
          '!mailfrom' => $mailfrom,
          '!account_page' => url("user/" . $email['userid'], array('absolute' => TRUE))
        )
      ));
    }

    // Additional check that user has access to publish content
    $node_type = variable_get('og_mailinglist_default_content_type', 'article');
    if (!user_access('create ' . $node_type . ' content', $user)) {
      throw new Exception(t("You do not have permission to post to this group. Please contact the site administrator."));
    }

    // See if the email is a comment to an existing node or a new node.
    // If the email is a comment to a node, we should be able to get the Node ID of the original node.
    $email['nid'] = og_mailinglist_parse_nid($email['text_plain'], $email['structure']->headers);

    // Create the new content in Drupal.
    if (!empty($email['nid'])) { // a new comment
      /* Two checks first before creating the comment.
       * There are at least two reasons why an email could have a nid but not be
       * intended as a new comment.
       * First, someone could be forwarding an email to a different group.
       * Second, it's common on mailinglists to fork threads by changing
       * the subject line. We need to check for both.
       */

      // Does the detected nid belong to the same group as the email was
      // forwarded to?
      // If not, then it was intended as start of a new thread.
      if (!og_is_member('node', $email['groupid'], 'node', $email['nid'])) {
        og_mailinglist_save_node($email);
        exit(0); // So we don't save a comment as well
      }

      // Check if the thread was forked by comparing the email subject to the node's title.
      // The levenshtein distance between a reply and node title should be 4 (e.g. "a subject" and "Re: a subject").
      // I added a few more to the distance to account for oddness in foreign languages but
      // is still small enough to catch any substantive subject change.
      $node = node_load($email['nid']);

      // Remove the group identifier (e.g. [a-group]) from the subject as it's not in the node title.
      $treated_subject =  preg_replace("/^[RrEe:]{3}\s\[[\w-]*\]/", "", $email['headers']['subject']);

      if (levenshtein($node->title, $treated_subject) > 7) {
        // The subject was changed so we'll create a new node now instead of a comment.
        // Unset the in-reply-to and references headers so this new thread doesn't get merged with the old thread.
        unset($email['headers']['in-reply-to']);
        unset($email['structure']->headers['in-reply-to']);
        unset($email['headers']['references']);
        unset($email['structure']->headers['references']);
        og_mailinglist_save_node($email);
        exit(0); // So we don't save a comment as well.
      }

      // If we got this far, the email is definitely intended as a new comment.
      og_mailinglist_save_comment($email);

    }
    else { // A new node.
      og_mailinglist_save_node($email);
    }

    // Tell Exim4 we had success!
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
      $msgfrom = $email['headers']['from'];
      $commentedbody = str_replace("\n", "\n> ", $email['mailbody']);
      $body = t("Hi, sorry about this but we were unable to handle your email correctly for the following reason:")
        . "\n\n"
        . $errormsg
        . "\n\n"
        . t("Please correct this error and try again, or contact the system administrator.  Thank you.")
        . "\n\n---- " . t("Below this line is a copy of the message") . " ----\n"
        . "> " . $commentedbody;

      // Send it off.
      if (!mail($email['headers']['from'], t("Error processing message"), $body, implode("\n", $head))) {
        throw new Exception("Mail error");
      }

      // print error message to log, then quit
      echo "Error: " . $e->getMessage() . "\n";
      exit(0);

    }
    catch (Exception $e2) {
      // if we get here, we couldn't even send an email back to sender, so just have Exim compose an error message and send back
      echo "Error: " . $e2->getMessage() . " ::: Embedded Error: " . $e->getMessage() . "\n";
      exit(1);
    }
  }
}

/**
 * Save new comment that came through email.
 */
function og_mailinglist_save_comment($email) {
  $nid = $email['nid'];
  $email['node'] = $node = node_load($nid);

  // Let other modules alter the new comment or delay posting for another time
  // (e.g. a moderation module might want to delay posting for a time).
  drupal_alter('og_mailinglist_new_email_comment', $email);

  // Other modules can unset certain emails to stop them from being saved and sent.
  // If they do, we'll just exit out.
  if (empty($email)) {
    exit(0);
  }

  // set the user account to this poster (comment_save checks the global user rights)
  global $user;
  $user = user_load($email['userid']);

  // check that this user has rights to post comments
  if (!user_access('post comments')) {
    throw new Exception(t("You do not have rights to post comments in the system."));
  }

  // Check that this post has comments enabled.
  // TODO: this is not neat.
  if ($node->comment != 1) {
    throw new Exception(t("Comments have been disabled for this post."));
  }

  $mailbody = $email['mailbody'];

  // Create an object representing the comment
  $comment = new stdClass();
  $comment->cid = 0;
  $comment->nid = $nid;
  $comment->uid = $email['userid'];
  $comment->language = LANGUAGE_NONE;
  $comment->is_anonymous = 0;
  $comment->status = COMMENT_PUBLISHED;
  $comment->node_type = 'comment_node_' . $node->type;

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

  // Get the cid that'll be used. Yes, this isn't a very pretty way to do this.
  // If someone else creates a comment between now and when the comment is
  // actually created, two emails will be sent out for this comment.
  $cid = 1 + db_query("SELECT cid FROM {comment} ORDER BY cid DESC LIMIT 1")->fetchField();

  // Log that comment came from email so og_mailinglist_phpmailer doesn't send an email as well.
  $parent_message_id = _og_mailinglist_get_thread_parent_messageid($email['headers']['in-reply-to'], $email['headers']['references']);
  og_mailinglist_log_email_sent('email', $nid, $cid, $email['headers']['message-id'], $email['headers']['in-reply-to'], $email['headers']['references'], $parent_message_id);

  // If enabled, set comment parent = cid of in-reply-to for comment threading
  if (variable_get('og_mailinglist_thread_comments', FALSE)) {
    $comment->thread = '01/';
    $comment->pid = db_query("SELECT cid FROM {og_mailinglist_source} WHERE message_id = :message_id", array(':message_id' => $email['headers']['in-reply-to']))->fetchField();
  }
  else {
    $comment->pid = 0;
  }

  // Let other modules alter the comment array before saving.
  drupal_alter('og_mailinglist_save_commment', $comment);

  // Save the new comment.
  comment_submit($comment);
  comment_save($comment);

  _og_mailinglist_email_comment_email($email, $node, $comment);

  $sender = user_load($email['userid']);
  $group_node = node_load($email['groupid']);
  // Report activity to watchdog.
  watchdog('og_mailinglist', "Posted comment for @mailfrom to group @group_name for nid=@nid with cid=@cid",
    array(
      '@mailfrom' => $sender->name,
      '@group_name' => $group_node->title,
      '@nid' => $node->nid,
      '@cid' => $cid,
    )
  );
}

/**
 * Save new nodes that came through email.
 */
function og_mailinglist_save_node($email) {

  // Let other modules alter the new node or delay posting for another time
  // (e.g. a moderation module might want to delay posting for a time).
  drupal_alter('og_mailinglist_new_email_node', $email);

  // Other modules can unset certain emails to stop them from being saved and sent.
  // If they do, we'll just exit out.
  if (empty($email)) {
    exit(0);
  }

  $mailbody = $email['mailbody'];

  // Get the nid that'll be used. Yes, this isn't a very pretty way to do this.
  // If someone else creates a node between now and when the node is
  // actually created, two emails will be sent out for this node.
  $nid = 1 + db_query("SELECT nid FROM {node} ORDER BY nid DESC LIMIT 1")->fetchField();

  // Log that comment came from email so og_mailinglist_phpmailer doesn't send an email as well.
  og_mailinglist_log_email_sent('email', $nid, 0, $email['headers']['message-id'], 0, 0, $email['headers']['message-id']);

  // Is the group public? If not, we need to set the node as private.
  // The og_private table stores a 1 if the group is private and 0 if it's public.
  // node_save() expects the opposite so we'll invert the value of $is_private.
  // TODO: Check whether it actually works.
  $is_public = TRUE;
  if (defined(OG_CONTENT_ACCESS_FIELD)) {
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
  $node->uid = $email['userid'];
  $node->language = LANGUAGE_NONE;
  $node->created = REQUEST_TIME;
  $node->body[$node->language][0]['value'] = $mailbody;
  $node->body[$node->language][0]['summary'] = text_summary($mailbody);
  $filter = variable_get('og_mailinglist_default_filter_format', 'plain_text');
  $node->body[$node->language][0]['format'] = $filter;
  // TODO: This needs love.
  //$node->status = 1; // published
  //$node->promote = 0;
  //$node->sticky = 0;
  //$node->og_public = $is_public;
  $node->og_group_ref[$node->language][]['target_id'] = $email['groupid'];
  $node->comment = variable_get("comment_$node_type", COMMENT_NODE_OPEN);

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

  // Send off email.
  _og_mailinglist_email_node_email($email, $node);

  $sender = user_load($email['userid']);
  $group_node = node_load($email['groupid']);
  // Report activity to watchdog.
  watchdog('og_mailinglist', "Posted node for @mailfrom to group @group_name with nid=@nid.",
    array(
      '@mailfrom' => $sender->name,
      '@group_name' => $group_node->title,
      '@nid' => $node->nid,
    )
  );
}

function _og_mailinglist_email_node_email($email, $node) {
  // Build new email.
  $email = _og_mailinglist_rewrite_headers($email, $node, TRUE);
  $footer = _og_mailinglist_build_footer($node);
  $email = _og_mailinglist_add_footer($email, $footer);
  $headers = $email['structure']->headers;
  unset($email['structure']->headers);
  $email['new_email_text'] = _og_mailinglist_encode_email(array($email['structure']));

  // Check if entire body should be encoded as base64
  $email['new_email_text'] = _og_mailinglist_check_base64($email['new_email_text'], $headers);

  // Send it off.
  $sender_account = user_load($email['userid']);
  _og_mailinglist_send_email($headers, $email['new_email_text'], $node, NULL, $sender_account);

  // If the sender's subscription type isn't email, give him a thread subscription.
  if (og_mailinglist_get_group_subscription_type($node->og_groups[0], $node->uid) != "email") {
    og_mailinglist_save_thread_subscriptions($node->nid, array($node->uid));
  }
}

function _og_mailinglist_email_comment_email($email, $node, $comment) {
  // Build new email.
  $email = _og_mailinglist_rewrite_headers($email, $node, FALSE, $comment);
  $footer = _og_mailinglist_build_footer($node);
  $email = _og_mailinglist_add_footer($email, $footer);
  $headers = $email['structure']->headers;
  unset($email['structure']->headers);
  $email['new_email_text'] = _og_mailinglist_encode_email(array($email['structure']));

  // Check if entire body should be encoded as base64
  $email['new_email_text'] = _og_mailinglist_check_base64($email['new_email_text'], $headers);

  // Send it off.
  $sender_account = user_load($email['userid']);
  _og_mailinglist_send_email($headers, $email['new_email_text'], $node, $comment, $sender_account);
}

function _og_mailinglist_parse_email($email) {
  $params['include_bodies'] = TRUE;
  $params['decode_bodies'] = TRUE;
  $params['decode_headers'] = TRUE;
  $params['input'] = $email['original_email_text'];

  // do the decode
  $email['structure'] = clone $structure = Mail_mimeDecode::decode($params);

  // Copy headers to $email array.
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
  // Make sure our text and html parts are accounted for
  if (isset($email['html']) && !isset($email['text'])) {
    $email['text'] = preg_replace('|<style.*</style>|mis', '', $email['html']);
    $email['text'] = drupal_html_to_text($email['text']);
  }
  elseif (isset($email['text']) && !isset($email['html'])) {
    $mail['html'] = check_markup($email['text']);
  }

  // Last ditch attempt - use the body as-is.
  if (!isset($email['text'])) {
    // Try to convert character set to UTF-8.
    if (!empty($structure->ctype_parameters['charset'])) {
      $email['text'] = drupal_convert_to_utf8($structure->body, $structure->ctype_parameters['charset']);
    }
    else {
      $email['text'] = $structure->body;
    }
    $email['html'] = check_markup($email['text']);
  }

  // Convert the subject to UTF-8.
  if (!empty($structure->ctype_parameters['charset'])) {
    $email['headers']['subject'] = drupal_convert_to_utf8($email['headers']['subject'], $structure->ctype_parameters['charset']);
  }

  // TODO figure out how to pull out sane HTML w/o funky css or other oddities.
  $email['mailbody'] = $email['text'];
  $email['isHTML'] = FALSE;

  // Save copy of the original mailbody
  $email['orig_mailbody'] = $email['mailbody'];

  return $email;
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
  $user = user_load($node->uid);

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
 * Take a raw attachment filename, decode it if necessary, and strip out invalid characters
 * Return a sanitised filename that should be ok for use by modules that want to save the file
 */
function _og_mailinglist_sanitise_filename($filename) {
  // Decode multibyte encoded filename
  $filename = mb_decode_mimeheader($filename);

  // Replaces all characters up through space and all past ~ along with the above reserved characters to sanitise filename
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
    if (empty($boundary)) {
      $boundary = $part->ctype_parameters['boundary'];
    }
    if (isset($part->parts)) {
      $email .= _og_mailinglist_encode_email_headers($part->headers) . "\n";
      $email .= "--" . $part->ctype_parameters['boundary'] . "\n";
      $email = _og_mailinglist_encode_email($part->parts, $part->ctype_parameters['boundary'], $email);
      $email .= "--" . $part->ctype_parameters['boundary'] . "--\n";
    }
    else {
      // Non-multipart emails don't have boundaries
      if ($boundary) {
        $last_line = array_pop(explode("\n", trim($email)));
        if (strcmp(trim($last_line), trim("--" . $boundary)) != 0) {
          $email .= "--" . $boundary . "\n";
        }
      }

      $email .= _og_mailinglist_encode_email_headers($part->headers) . "\n";
      // Encode the body as base64 if necessary
      if ($part->headers['content-transfer-encoding'] == "base64") {
        $email .= wordwrap(base64_encode($part->body), 76, "\n", TRUE);
        $email .= "\n";
      }
      else {
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
function _og_mailinglist_rewrite_headers($email, $node, $new_node = FALSE, $comment = 0) {
  $gids = og_mailinglist_get_groups($node);
  if (empty($gids)) {
    exit();
  }
  // TODO: This one here looks like a slight hack.
  $group_nodes = node_load_multiple($gids);
  $group_node = current($group_nodes);

  $group_name = _og_mailinglist_get_group_name($group_node);
  $server = variable_get("og_mailinglist_server_string", $_SERVER['SERVER_NAME']);
  $group_email = $group_name . '@' . $server;

  $headers = $email['structure']->headers;
  $new_headers = array();
  $new_headers['mime-version'] = $headers['mime-version'];
  $new_headers['date'] = $headers['date'];
  if ($new_node) {
    $new_headers['subject'] = "[" . $group_email . "] " . $node->title;
  }
  else {
    $new_headers['subject'] = $headers['subject'];
  }

  $new_headers['from'] = $headers['from'];
  $new_headers['to'] = $group_email;
  $new_headers['bcc'] = _og_mailinglist_get_subscribers($node, $group_node, $new_node);
  $new_headers['content-type'] = $headers['content-type'];
  $new_headers['content-transfer-encoding'] =  $headers['content-transfer-encoding'];

  // Add a reply-to header directed to the group if enabled.
  if (variable_get('og_mailinglist_reply_to_group', FALSE)) {
    $new_headers['reply-to'] = $group_email;
  }

  // Add list headers.
  $new_headers['List-Id'] = "<" . $group_email . ">";
  $new_headers['List-Post'] = "<mailto:" . $group_email . ">";
  $new_headers['List-Archive'] = url("node/" . $group_node->nid, array('absolute' => TRUE));

  // Thread-URL header.
  global $base_url;
  $new_headers['X-Thread-Url'] = $base_url . "/node/" . $node->nid;

  // Set x-BeenThere header
  $new_headers['X-BeenThere'] = $group_email;

  // Set Message-Id
  $new_headers['Message-ID'] = $email['headers']['message-id'];

  // Set References
  $new_headers['References'] = $email['headers']['references'];

  // Set In-Reply-To.
  $new_headers['in-reply-to'] = $email['headers']['in-reply-to'];

  $email['structure']->headers = $new_headers;

  return $email;
}

function _og_mailinglist_check_base64($body, $headers) {
  // If text/plain + base64
  if ($headers['content-transfer-encoding'] == 'base64') {
    return wordwrap(base64_encode($body), 76, "\n", TRUE) . "\n";
  }
  return $body;
}

function _og_mailinglist_add_footer($email, $footer) {
  $headers = $email['structure']->headers;
  $structure = $email['structure'];

  // If message is 7/8bit text/plain and uses us-ascii charecter set, just
  // append the footer.
  if (preg_match('/^text\/plain/i', $headers['content-type']) &&
      isset($structure->body)) {
    $structure->body .= "\n" . $footer;
  }
  // If message is already multipart, just append new part with footer to end
  // /^multipart\/(mixed|related)/i
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
    // and append footer field to end.
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

function _og_mailinglist_send_email($headers, $body, $node, $comment = NULL, $sender_account) {
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
        if (preg_match("/(^<)*<(.*?)>/", $value, $matches)) {
          $mailer->AddAddress($matches[1], $matches[2]);
        }
        else {
          $mailer->AddAddress($value);
        }
        break;
      case 'cc':
        if (preg_match("/(^<)*<(.*?)>/", $value, $matches)) {
          $mailer->AddCC($matches[1], $matches[2]);
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
        if (preg_match("/([^<]*)<(.*?)>/", $value, $matches)) {
          $mailer->From = $matches[2];
          $mailer->FromName = $matches[1];
        }
        else {
          $mailer->From = $value;
          $mailer->FromName = ($sender_account->realname) ? $sender_account->realname : $sender_account->name;
        }
        break;
      case 'content-type':
        $mailer->ContentType = $value;

        // Parse out the charset if one is set.
        preg_match("/charset=(.*)/s", $value, $matches);
        $charset = $matches[1];
        // If we succeed, set that for the mailer than proceed.
        if (!empty($charset)) {
          $mailer->CharSet = $charset;
        }
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
 * @param $email
 *  Email address. Not used right now.
 *
 * @param $headers
 *  Headers of the email to be parsed.
 */
function og_mailinglist_parse_nid($email, $headers) {
  $nid = "";
  // Check the in-reply-to header.
  $parent_message_id = _og_mailinglist_get_thread_parent_messageid($headers['in-reply-to'], $headers['references']);
  if (!empty($parent_message_id)) {
    $nid = _og_mailinglist_get_nid_from_parent_messageid($parent_message_id);
  }

  if (!empty($nid)) {
    return $nid;
  }
  // Else, as last resort, pick the last node that matches the title in the subject line.
  else {
    $nid = _og_mailinglist_nid_of_subject($headers['subject']);
  }
  return $nid;
}

function _og_mailinglist_nid_of_subject($subject) {
  $nid = "";
  preg_match("/[Rr][Ee]:\s\[.*?\]\s(.+)/s", $subject, $matches);
  $title = $matches[1];
  if ($title) {
    $nid = db_query("SELECT nid
                       FROM {node}
                       WHERE title LIKE :title
                       ORDER BY created desc
                       LIMIT 1", array(':title' => '%' . $title . '%'))->fetchField();
  }
  return $nid;
}
