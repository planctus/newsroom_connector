<?php

/**
 * @file
 * Class definition of FeedsNodeProcessor.
 */

/**
 * Creates nodes from feed items.
 */
class NewsroomDeleteNodeProcessor extends FeedsProcessor {

  /**
   * Define entity type.
   */
  public function entityType() {
    return 'node';
  }

  /**
   * Implements parent::entityInfo().
   */
  protected function entityInfo() {
    $info = parent::entityInfo();
    $info['label plural'] = t('Nodes');
    return $info;
  }

  /**
   * Creates a new node in memory and returns it.
   */
  protected function newEntity(FeedsSource $source) {
    $node = new stdClass();
    $node->type = $this->config['content_type'];
    $node->changed = REQUEST_TIME;
    $node->created = REQUEST_TIME;
    $node->language = LANGUAGE_NONE;
    node_object_prepare($node);
    // Populate properties that are set by node_object_prepare().
    $node->log = 'Created by FeedsNodeProcessor';
    $node->uid = $this->config['author'];
    return $node;
  }

  /**
   * Loads an existing node.
   *
   * If the update existing method is not FEEDS_UPDATE_EXISTING, only the node
   * table will be loaded, foregoing the node_load API for better performance.
   *
   * @todo Reevaluate the use of node_object_prepare().
   */
  protected function entityLoad(FeedsSource $source, $nid) {
    if ($this->config['update_existing'] == FEEDS_UPDATE_EXISTING) {
      $node = node_load($nid, NULL, TRUE);
    }
    else {
      // We're replacing the existing node. Only save the absolutely necessary.
      $node = db_query("SELECT created, nid, vid, type FROM {node} WHERE nid = :nid", array(':nid' => $nid))->fetchObject();
      $node->uid = $this->config['author'];
    }
    node_object_prepare($node);

    // Workaround for issue #1247506. See #1245094 for backstory.
    if (!empty($node->menu)) {
      // If the node has a menu item(with a valid mlid) it must be flagged
      // 'enabled'.
      $node->menu['enabled'] = (int) (bool) $node->menu['mlid'];
    }

    // Populate properties that are set by node_object_prepare().
    if ($this->config['update_existing'] == FEEDS_UPDATE_EXISTING) {
      $node->log = 'Updated by FeedsNodeProcessor';
    }
    else {
      $node->log = 'Replaced by FeedsNodeProcessor';
    }
    return $node;
  }

  /**
   * Save a node.
   */
  public function entitySave($entity) {
    // If nid is set and a node with that id doesn't exist, flag as new.
    if (!empty($entity->nid) && !node_load($entity->nid)) {
      $entity->is_new = TRUE;
    }
  }

  /**
   * Delete a series of nodes.
   */
  protected function entityDeleteMultiple($nids) {
    node_delete_multiple($nids);
  }

  /**
   * Override parent::configDefaults().
   */
  public function configDefaults() {
    $types = node_type_get_names();
    $type = isset($types['article']) ? 'article' : key($types);
    return array(
      'content_type' => $type,
      'expire' => FEEDS_EXPIRE_NEVER,
      'author' => 0,
    ) + parent::configDefaults();
  }

  /**
   * Override parent::configForm().
   */
  public function configForm(&$form_state) {
    $types = node_type_get_names();
    array_walk($types, 'check_plain');
    $form = parent::configForm($form_state);
    $form['content_type'] = array(
      '#type' => 'select',
      '#title' => t('Content type'),
      '#description' => t('Select the content type for the nodes to be created. <strong>Note:</strong> Users with "import !feed_id feeds" permissions will be able to <strong>import</strong> nodes of the content type selected here regardless of the node level permissions. Further, users with "clear !feed_id permissions" will be able to <strong>delete</strong> imported nodes regardless of their node level permissions.', array('!feed_id' => $this->id)),
      '#options' => $types,
      '#default_value' => $this->config['content_type'],
    );
    $author = user_load($this->config['author']);
    $form['author'] = array(
      '#type' => 'textfield',
      '#title' => t('Author'),
      '#description' => t('Select the author of the nodes to be created - leave empty to assign "anonymous".'),
      '#autocomplete_path' => 'user/autocomplete',
      '#default_value' => empty($author->name) ? 'anonymous' : check_plain($author->name),
    );
    $period = drupal_map_assoc(
        array(FEEDS_EXPIRE_NEVER, 3600, 10800, 21600, 43200, 86400,
          259200, 604800, 2592000, 2592000 * 3,
          2592000 * 6, 31536000,
        ),
        'feeds_format_expire');
    $form['expire'] = array(
      '#type' => 'select',
      '#title' => t('Expire nodes'),
      '#options' => $period,
      '#description' => t("Select after how much time nodes should be deleted. The node's published date will be used for determining the node's age, see Mapping settings."),
      '#default_value' => $this->config['expire'],
    );
    $form['update_existing']['#options'] = array(
      FEEDS_SKIP_EXISTING => 'Do not update existing nodes',
      FEEDS_REPLACE_EXISTING => 'Replace existing nodes',
      FEEDS_UPDATE_EXISTING => 'Update existing nodes (slower than replacing them)',
    );
    return $form;
  }

  /**
   * Override parent::configFormValidate().
   */
  public function configFormValidate(&$values) {
    if ($author = user_load_by_name($values['author'])) {
      $values['author'] = $author->uid;
    }
    else {
      $values['author'] = 0;
    }
  }

  /**
   * Reschedule if expiry time changes.
   */
  public function configFormSubmit(&$values) {
    if ($this->config['expire'] != $values['expire']) {
      feeds_reschedule($this->id);
    }
    parent::configFormSubmit($values);
  }

  /**
   * Process the result of the parsing stage.
   *
   * @param FeedsSource $source
   *   Source information about this import.
   * @param FeedsParserResult $parser_result
   *   The result of the parsing stage.
   */
  public function process(FeedsSource $source, FeedsParserResult $parser_result) {
    $state = $source->state(FEEDS_PROCESS);

    while ($item = $parser_result->shiftItem()) {
      if (!($entity_ids = $this->existingEntityId($source, $parser_result)) ||
          ($this->config['update_existing'] != FEEDS_SKIP_EXISTING)) {

        try {
          if (!empty($entity_ids)) {
            $this->entityDeleteMultiple($entity_ids);
            $state->deleted++;
          }
        }
        catch (Exception $e) {
          $state->failed++;
          drupal_set_message($e->getMessage(), 'warning');
          $message = $e->getMessage();
          $message .= '<h3>Original item</h3>';
          $message .= '<pre>' . var_export($item, TRUE) . '</pre>';
          $message .= '<h3>Entity</h3>';
          $message .= '<pre>' . var_export($entity, TRUE) . '</pre>';
          $source->log('import', $message, array(), WATCHDOG_ERROR);
        }
      }
    }

    // Set messages if we're done.
    if ($source->progressImporting() != FEEDS_BATCH_COMPLETE) {
      return;
    }
    $info = $this->entityInfo();
    $tokens = array(
      '@entity' => drupal_strtolower($info['label']),
      '@entities' => drupal_strtolower($info['label plural']),
    );
    $messages = array();
    if ($state->deleted) {
      $messages[] = array(
        'message' => format_plural(
            $state->created, 'Deleted @number @entity.', 'Deleted @number @entities.', array('@number' => $state->deleted) + $tokens
        ),
      );
    }
    if ($state->failed) {
      $messages[] = array(
        'message' => format_plural(
            $state->failed, 'Failed deleting @number @entity.', 'Failed deleting @number @entities.', array('@number' => $state->failed) + $tokens
        ),
        'level' => WATCHDOG_ERROR,
      );
    }
    if (empty($messages)) {
      $messages[] = array(
        'message' => t('There are no new @entities to delete.', array('@entities' => drupal_strtolower($info['label plural']))),
      );
    }
    foreach ($messages as $message) {
      drupal_set_message($message['message']);
      $source->log('import', $message['message'], array(), isset($message['level']) ? $message['level'] : WATCHDOG_INFO);
    }
  }

  /**
   * Override setTargetElement to operate on a target item that is a node.
   */
  public function setTargetElement(FeedsSource $source, $target_node, $target_element, $value) {
    switch ($target_element) {
      case 'created':
        $target_node->created = feeds_to_unixtime($value, REQUEST_TIME);
        break;

      case 'feeds_source':
        // Get the class of the feed node importer's fetcher and set the source
        // property. See feeds_node_update() how $node->feeds gets stored.
        if ($id = feeds_get_importer_id($this->config['content_type'])) {
          $class = get_class(feeds_importer($id)->fetcher);
          $target_node->feeds[$class]['source'] = $value;
          // This effectively suppresses 'import on submission' feature.
          // See feeds_node_insert().
          $target_node->feeds['suppress_import'] = TRUE;
        }
        break;

      default:
        parent::setTargetElement($source, $target_node, $target_element, $value);
        break;
    }
  }

  /**
   * Return available mapping targets.
   */
  public function getMappingTargets() {
    $type = node_type_get_type($this->config['content_type']);
    $targets = parent::getMappingTargets();
    if ($type->has_title) {
      $targets['title'] = array(
        'name' => t('Title'),
        'description' => t('The title of the node.'),
        'optional_unique' => TRUE,
      );
    }
    $targets['nid'] = array(
      'name' => t('Node ID'),
      'description' => t('The nid of the node. NOTE: use this feature with care, node ids are usually assigned by Drupal.'),
      'optional_unique' => TRUE,
    );
    $targets['uid'] = array(
      'name' => t('User ID'),
      'description' => t('The Drupal user ID of the node author.'),
    );
    $targets['status'] = array(
      'name' => t('Published status'),
      'description' => t('Whether a node is published or not. 1 stands for published, 0 for not published.'),
    );
    $targets['created'] = array(
      'name' => t('Published date'),
      'description' => t('The UNIX time when a node has been published.'),
    );
    $targets['promote'] = array(
      'name' => t('Promoted to front page'),
      'description' => t('Boolean value, whether or not node is promoted to front page. (1 = promoted, 0 = not promoted)'),
    );
    $targets['sticky'] = array(
      'name' => t('Sticky'),
      'description' => t('Boolean value, whether or not node is sticky at top of lists. (1 = sticky, 0 = not sticky)'),
    );

    // Include language field if Locale module is enabled.
    if (module_exists('locale')) {
      $targets['language'] = array(
        'name' => t('Language'),
        'description' => t('The two-character language code of the node.'),
      );
    }

    // Include comment field if Comment module is enabled.
    if (module_exists('comment')) {
      $targets['comment'] = array(
        'name' => t('Comments'),
        'description' => t('Whether comments are allowed on this node: 0 = no, 1 = read only, 2 = read/write.'),
      );
    }

    // If the target content type is a Feed node, expose its source field.
    if ($id = feeds_get_importer_id($this->config['content_type'])) {
      $name = feeds_importer($id)->config['name'];
      $targets['feeds_source'] = array(
        'name' => t('Feed source'),
        'description' => t('The content type created by this processor is a Feed Node, it represents a source itself. Depending on the fetcher selected on the importer "@importer", this field is expected to be for example a URL or a path to a file.', array('@importer' => $name)),
        'optional_unique' => TRUE,
      );
    }

    // Let other modules expose mapping targets.
    self::loadMappers();
    feeds_alter('feeds_processor_targets', $targets, 'node', $this->config['content_type']);

    return $targets;
  }

  /**
   * Get nid of an existing feed item node if available.
   */
  protected function existingEntityId(FeedsSource $source, FeedsParserResult $result) {
    // Iterate through all unique targets and test whether they do already
    // exist in the database.
    $nids = array();
    foreach ($this->uniqueTargets($source, $result) as $target => $value) {
      switch ($target) {
        case 'guid':
          $result_nid = db_query("SELECT entity_id FROM {feeds_item} WHERE guid = :guid", array(':guid' => $value));
          foreach ($result_nid as $record) {
            $nids[] = $record->entity_id;
          }
          break;
      }
      if (count($nids) > 0) {
        // Return with the first nid found.
        return $nids;
      }
    }
    return 0;
  }

}
